/**
 * @file search_env.cpp
 * @brief Object recognition search environment
 * @author Venkatraman Narayanan
 * Carnegie Mellon University, 2015
 */

#include <sbpl_perception/search_env.h>

#include <perception_utils/perception_utils.h>
#include <sbpl_perception/discretization_manager.h>
#include <kinect_sim/camera_constants.h>
// #include <sbpl_perception/utils/object_utils.h>

#include <ros/ros.h>
#include <ros/package.h>

#include <pcl/conversions.h>
#include <pcl/filters/filter.h>
#include <pcl_ros/transforms.h>
#include <pcl/PCLPointCloud2.h>
#include <pcl_conversions/pcl_conversions.h>

#include <pcl/io/io.h>
#include <pcl/io/pcd_io.h>
#include <pcl/io/png_io.h>
#include <pcl/io/vtk_lib_io.h>
#include <pcl/common/common.h>
#include <pcl/console/print.h>

#include <opencv/highgui.h>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/core/eigen.hpp>
//// #include <opencv2/contrib/contrib.hpp>

#include <boost/lexical_cast.hpp>
#include <omp.h>
#include <algorithm>
#include <pcl/point_cloud.h>
#include <pcl/octree/octree2buf_base.h>
#include <pcl/octree/octree_pointcloud_changedetector.h>
using namespace std;
using namespace perception_utils;
using namespace pcl::simulation;
using namespace Eigen;

namespace {
  // Whether should use depth-dependent cost penalty. If true, cost is
  // indicator(pixel explained) * multiplier * range_in_meters(pixel). Otherwise, cost is
  // indicator(pixel explained).
  constexpr bool kUseDepthSensitiveCost = false;
  // The multiplier used in the above definition.
  constexpr double kDepthSensitiveCostMultiplier = 100.0;
  // If true, we will use a normalized outlier cost function.
  constexpr bool kNormalizeCost = true;
  // When use_clutter is true, we will treat every pixel that is not within
  // the object volume as an extraneous occluder if its depth is less than
  // depth(rendered pixel) - kOcclusionThreshold.
  constexpr unsigned short kOcclusionThreshold = 50; // mm
  // Tolerance used when deciding the footprint of the object in a given pose is
  // out of bounds of the supporting place.
  constexpr double kFootprintTolerance = 0.2; // m 0.15 for crate

  // Max color distance for two points to be considered neighbours
  // constexpr double kColorDistanceThreshold = 7.5; // m
  constexpr double kColorDistanceThresholdCMC = 10; // m

  // constexpr double kColorDistanceThreshold = 20; // m

  constexpr double kNormalizeCostBase = 100;

  // bool kUseColorCost = true;

  bool kUseColorPruning = false;

  bool kUseHistogramPruning = false;

  bool kUseHistogramLazy = false;

  double kHistogramLazyScoreThresh = 0.8;

  bool kUseOctomapPruning = false;

  bool cost_debug_msgs = true;

}  // namespace

namespace sbpl_perception {

EnvObjectRecognition::EnvObjectRecognition(const
                                           std::shared_ptr<boost::mpi::communicator> &comm) :
  mpi_comm_(comm),
  image_debug_(false), debug_dir_(ros::package::getPath("sbpl_perception") +
                                  "/visualization/"), env_stats_ {0, 0, 0, 0, 0} {

  // OpenGL requires argc and argv
  char **argv;
  argv = new char *[2];
  argv[0] = new char[1];
  argv[1] = new char[1];
  argv[0] = const_cast<char *>("0");
  argv[1] = const_cast<char *>("1");

  // printf("Making simulator camera and input camera dimensions equal\n");
  // kCameraHeight = kCameraHeight;
  // kCameraWidth = kCameraWidth;
  // kNumPixels = kCameraWidth * kCameraHeight;

  
  observed_cloud_.reset(new PointCloud);
  original_input_cloud_.reset(new PointCloud);
  constraint_cloud_.reset(new PointCloud);
  projected_constraint_cloud_.reset(new PointCloud);
  projected_cloud_.reset(new PointCloud);
  observed_organized_cloud_.reset(new PointCloud);
  downsampled_observed_cloud_.reset(new PointCloud);
  downsampled_projected_cloud_.reset(new PointCloud);

  gl_inverse_transform_ <<
                        0, 0 , -1 , 0,
                        -1, 0 , 0 , 0,
                        0, 1 , 0 , 0,
                        0, 0 , 0 , 1;

  pcl::console::setVerbosityLevel(pcl::console::L_ALWAYS);

  // Load algorithm parameters.
  // NOTE: Do not change these default parameters. Configure these in the
  // appropriate yaml file.
  if (IsMaster(mpi_comm_)) {
    if (!ros::isInitialized()) {
      printf("Error: ros::init() must be called before environment can be constructed\n");
      mpi_comm_->abort(1);
      exit(1);
    }

    ros::NodeHandle private_nh("~");


    // ros::NodeHandle nh_;
    input_point_cloud_topic = private_nh.advertise<sensor_msgs::PointCloud2>("/perch/input_point_cloud", 1);
    
    gpu_input_point_cloud_topic = private_nh.advertise<sensor_msgs::PointCloud2>("/perch/gpu_input_point_cloud", 1);

    downsampled_input_point_cloud_topic = private_nh.advertise<sensor_msgs::PointCloud2>("/perch/downsampled_input_point_cloud", 1);

    render_point_cloud_topic = private_nh.advertise<sensor_msgs::PointCloud2>("/perch/rendered_point_cloud", 1);

    // std::string downsampled_mesh_cloud_topic_name = "/perch/downsampled_mesh_cloud_" + obj_models_[model_id].name();
    std::string downsampled_mesh_cloud_topic_name = "/perch/downsampled_mesh_cloud";
    downsampled_mesh_cloud_topic = private_nh.advertise<sensor_msgs::PointCloud2>(downsampled_mesh_cloud_topic_name, 1);

    private_nh.param("/perch_params/sensor_resolution_radius", perch_params_.sensor_resolution,
                     0.003);
    private_nh.param("/perch_params/min_neighbor_points_for_valid_pose",
                     perch_params_.min_neighbor_points_for_valid_pose, 50);
    private_nh.param("/perch_params/min_points_for_constraint_cloud",
                     perch_params_.min_points_for_constraint_cloud, 50);
    private_nh.param("/perch_params/max_icp_iterations", perch_params_.max_icp_iterations, 10);
    private_nh.param("/perch_params/icp_max_correspondence",
                     perch_params_.icp_max_correspondence, 0.05);
    private_nh.param("/perch_params/use_adaptive_resolution",
                     perch_params_.use_adaptive_resolution, false);
    private_nh.param("/perch_params/use_rcnn_heuristic", perch_params_.use_rcnn_heuristic, false);
    private_nh.param("/perch_params/use_model_specific_search_resolution",
                     perch_params_.use_model_specific_search_resolution, false);
    private_nh.param("/perch_params/use_clutter_mode", perch_params_.use_clutter_mode, false);
    private_nh.param("/perch_params/clutter_regularizer", perch_params_.clutter_regularizer, 1.0);
    private_nh.param("/perch_params/use_downsampling", perch_params_.use_downsampling, false);
    private_nh.param("/perch_params/downsampling_leaf_size", perch_params_.downsampling_leaf_size, 0.01);

    private_nh.param("/perch_params/visualize_expanded_states",
                     perch_params_.vis_expanded_states, false);
    private_nh.param("/perch_params/visualize_successors",
                     perch_params_.vis_successors, false);
    private_nh.param("/perch_params/print_expanded_states", perch_params_.print_expanded_states,
                     false);
    private_nh.param("/perch_params/debug_verbose", perch_params_.debug_verbose, false);
    private_nh.param("/perch_params/use_color_cost", perch_params_.use_color_cost, false);
    private_nh.param("/perch_params/gpu_batch_size", perch_params_.gpu_batch_size, 1000);
    private_nh.param("/perch_params/use_gpu", perch_params_.use_gpu, true);
    private_nh.param("/perch_params/color_distance_threshold", perch_params_.color_distance_threshold, 20.0);
    private_nh.param("/perch_params/gpu_stride", perch_params_.gpu_stride, 8.0);
    private_nh.param("/perch_params/use_cylinder_observed", perch_params_.use_cylinder_observed, true);
    private_nh.param("/perch_params/gpu_occlusion_threshold", perch_params_.gpu_occlusion_threshold, 1.0);
    private_nh.param("/perch_params/footprint_tolerance", perch_params_.footprint_tolerance, 0.05);
    private_nh.param("/perch_params/depth_median_blur", perch_params_.depth_median_blur, 17.0);
    private_nh.param("/perch_params/icp_type", perch_params_.icp_type, 0); // 0 - PCL 2d icp, 1 - gicp cpu 3d, 2 - gicp cuda 3d
    perch_params_.initialized = true;

    printf("----------PERCH Config-------------\n");
    printf("Sensor Resolution Radius: %f\n", perch_params_.sensor_resolution);
    printf("Min Points for Valid Pose: %d\n",
           perch_params_.min_neighbor_points_for_valid_pose);
    printf("Min Points for Constraint Cloud: %d\n",
           perch_params_.min_points_for_constraint_cloud);
    printf("Max ICP Iterations: %d\n", perch_params_.max_icp_iterations);
    printf("ICP Max Correspondence: %f\n", perch_params_.icp_max_correspondence);
    printf("Use Model-specific Search Resolution: %d\n",
           static_cast<int>(perch_params_.use_model_specific_search_resolution));
    printf("RCNN Heuristic: %d\n", perch_params_.use_rcnn_heuristic);
    printf("Use Clutter: %d\n", perch_params_.use_clutter_mode);
    printf("Clutter Regularization: %f\n", perch_params_.clutter_regularizer);
    printf("Use Dowsampling: %d\n", perch_params_.use_downsampling);
    printf("Dowsampling Leaf Size: %f\n", perch_params_.downsampling_leaf_size);
    printf("Vis Expansions: %d\n", perch_params_.vis_expanded_states);
    printf("Vis Successors: %d\n", perch_params_.vis_successors);
    printf("Print Expansions: %d\n", perch_params_.print_expanded_states);
    printf("Debug Verbose: %d\n", perch_params_.debug_verbose);
    printf("Use Color Cost: %d\n", perch_params_.use_color_cost);
    printf("Color Distance Threshold: %f\n", perch_params_.color_distance_threshold);
    printf("Use GPU: %d\n", perch_params_.use_gpu);
    printf("GPU batch size: %d\n", perch_params_.gpu_batch_size);
    printf("GPU stride: %f\n", perch_params_.gpu_stride);
    printf("Use Cylinder Observed: %d\n", perch_params_.use_cylinder_observed);
    printf("Footprint Tolerance: %f\n", perch_params_.footprint_tolerance);
    printf("Depth Median Blur: %f\n", perch_params_.depth_median_blur);
    printf("\n");
    printf("----------Camera Config-------------\n");
    printf("Camera Width: %d\n", kCameraWidth);
    printf("Camera Height: %d\n", kCameraHeight);
    printf("Camera FX: %f\n", kCameraFX);
    printf("Camera FY: %f\n", kCameraFY);
    printf("Camera CX: %f\n", kCameraCX);
    printf("Camera CY: %f\n", kCameraCY);
    env_params_.cam_intrinsic=(cv::Mat_<float>(3,3) << kCameraFX, 0.0, kCameraCX, 0.0, kCameraFY, kCameraCY, 0.0, 0.0, 1.0);
    env_params_.width = kCameraWidth;
    env_params_.height = kCameraHeight;
    env_params_.proj_mat = cuda_renderer::compute_proj(env_params_.cam_intrinsic, env_params_.width, env_params_.height);
  }

  mpi_comm_->barrier();
  broadcast(*mpi_comm_, perch_params_, kMasterRank);
  assert(perch_params_.initialized);
  
  if (!perch_params_.initialized) {
    printf("ERROR: PERCH Params not initialized for process %d\n",
           mpi_comm_->rank());
  }
  
  if (perch_params_.use_gpu == 0)
  {
    kinect_simulator_ = SimExample::Ptr(new SimExample(0, argv,
                                                      kCameraHeight, kCameraWidth));
    scene_ = kinect_simulator_->scene_;
  }

}

EnvObjectRecognition::~EnvObjectRecognition() {
}

void EnvObjectRecognition::LoadObjFiles(const ModelBank
                                        &model_bank,
                                        const vector<string> &model_names) {

  assert(model_bank.size() >= model_names.size());

  // TODO: assign all env params in a separate method
  env_params_.num_models = static_cast<int>(model_names.size());

  obj_models_.clear();
  tris.clear();
  tris_model_count.clear();
  for (int ii = 0; ii < env_params_.num_models; ++ii) {
    string model_name = model_names[ii];
    auto model_bank_it = model_bank.find(model_name);

    if (model_bank_it == model_bank.end()) {
      printf("Model %s not found in model bank\n", model_name.c_str());
      exit(1);
    }

    const ModelMetaData &model_meta_data = model_bank_it->second;

    pcl::PolygonMesh mesh;
    pcl::io::loadPolygonFilePLY (model_meta_data.file.c_str(), mesh);
    
    
    ObjectModel obj_model(mesh, 
                          model_meta_data.name,
                          model_meta_data.symmetric,
                          model_meta_data.flipped,
                          env_params_.use_external_pose_list);
    obj_models_.push_back(obj_model);

    if (IsMaster(mpi_comm_)) {
      printf("Read %s with %d polygons and %d triangles from file %s\n", model_name.c_str(),
             static_cast<int>(mesh.polygons.size()),
             static_cast<int>(mesh.cloud.data.size()),
             model_meta_data.file.c_str());
      printf("Object dimensions: X: %f %f, Y: %f %f, Z: %f %f, Rad: %f,   %f\n",
             obj_model.min_x(),
             obj_model.max_x(), obj_model.min_y(), obj_model.max_y(), obj_model.min_z(),
             obj_model.max_z(), obj_model.GetCircumscribedRadius(),
             obj_model.GetInscribedRadius());
      printf("\n");
    }
    if (perch_params_.use_gpu)
    {
        cuda_renderer::Model render_model(model_meta_data.file.c_str()); 
        render_models_.push_back(render_model);
        tris.insert(tris.end(), render_model.tris.begin(), render_model.tris.end());
        tris_model_count.push_back(render_model.tris.size());
    }
  }
}

bool EnvObjectRecognition::IsValidPose(GraphState s, int model_id,
                                       ContPose pose, bool after_refinement = false,
                                       int required_object_id = -1) const {



  vector<int> indices;
  vector<float> sqr_dists;
  PointT point;

  point.x = pose.x();
  point.y = pose.y();

  // if (env_params_.use_external_render == 1)
  //   point.z = pose.z() - 0.086;
  // else
  point.z = pose.z();

  // std::cout << "x:" << point.x << "y:" << point.y << "z:" << point.z << endl;
  // point.z = (obj_models_[model_id].max_z()  - obj_models_[model_id].min_z()) /
  //           2.0 + env_params_.table_height;

  double grid_cell_circumscribing_radius = 0.0;

  auto it = model_bank_.find(obj_models_[model_id].name());
  assert (it != model_bank_.end());
  const auto &model_meta_data = it->second;
  double search_resolution = 0;

  if (perch_params_.use_model_specific_search_resolution) {
    search_resolution = model_meta_data.search_resolution;
  } else {
    search_resolution = env_params_.res;
  }

  if (after_refinement) {
    grid_cell_circumscribing_radius = 0.0;
  } else {
    grid_cell_circumscribing_radius = std::hypot(search_resolution / 2.0,
                                                 search_resolution / 2.0);
  }



  // if (env_params_.use_external_render == 1) {
  //     // Axis is different
  //     search_rad = 0.5 * search_rad;
  // }

  int min_neighbor_points_for_valid_pose = perch_params_.min_neighbor_points_for_valid_pose;
  int num_neighbors_found = 0;
  double search_rad;

  if (env_params_.use_external_pose_list != 1)
  {
    search_rad = std::max(
                            obj_models_[model_id].GetCircumscribedRadius(),
                            grid_cell_circumscribing_radius);
    num_neighbors_found = projected_knn_->radiusSearch(point, search_rad,
                                                        indices,
                                                        sqr_dists, min_neighbor_points_for_valid_pose); //0.2
  }
  else
  {
    // For 6D cant search in projected cloud so search in original cloud
    search_rad = std::max(
                            obj_models_[model_id].GetInflationFactor() *
                            obj_models_[model_id].GetCircumscribedRadius3D(),
                            grid_cell_circumscribing_radius);
    if (required_object_id == -1)
    {
      num_neighbors_found = knn->radiusSearch(point, search_rad,
                                              indices,
                                              sqr_dists, min_neighbor_points_for_valid_pose); //0.2
    }
    else
    {
      // search only in segmented cloud for this object
      num_neighbors_found = segmented_object_knn[required_object_id]->radiusSearch(point, search_rad,
                                        indices,
                                        sqr_dists, min_neighbor_points_for_valid_pose); //0.2
    }
  }



  // int min_neighbor_points_for_valid_pose = 50;
  // int num_neighbors_found = downsampled_projected_knn_->radiusSearch(point, search_rad,
  //                                                        indices,
  //                                                        sqr_dists, min_neighbor_points_for_valid_pose); //0.2


  if (num_neighbors_found < min_neighbor_points_for_valid_pose) {
    // printf("Invalid 1, neighbours found : %d, radius %f\n",num_neighbors_found, search_rad);
    return false;
  }
  else {
    // PrintPointCloud(obj_models_[model_id].downsampled_mesh_cloud(), 1, downsampled_mesh_cloud_topic);
    // sensor_msgs::PointCloud2 output;
    // pcl::PCLPointCloud2 outputPCL;
    // pcl::toPCLPointCloud2( *obj_models_[model_id].downsampled_mesh_cloud() ,outputPCL);
    // pcl::toPCLPointCloud2( *downsampled_projected_cloud_ ,outputPCL);
    // pcl_conversions::fromPCL(outputPCL, output);
    // output.header.frame_id = env_params_.reference_frame_;
    // downsampled_mesh_cloud_topic.publish(output);

    if (perch_params_.use_color_cost && !after_refinement && kUseColorPruning) {
      // printf("Color pruning for model : %s\n", obj_models_[model_id].name().c_str());
      int total_num_color_neighbors_found = 0;
      for (int i = 0; i < indices.size(); i++)
      {
        // Find color matching points in observed_color point cloud
        int num_color_neighbors_found =
            getNumColorNeighboursCMC(
              downsampled_projected_cloud_->points[indices[i]], obj_models_[model_id].downsampled_mesh_cloud()
            );
        total_num_color_neighbors_found += num_color_neighbors_found;
        // if (num_color_neighbors_found == 0) {
        //   return false;
        // }
      }

      if ((double)total_num_color_neighbors_found/indices.size() < 0.3) {
          // printf("Total color neighbours found : %d\n", total_num_color_neighbors_found);
          // printf("Fraction of points with color neighbours found : %f\n", (double)total_num_color_neighbors_found/indices.size());
          return false;
      } else {
        printf("Total color neighbours found : %d\n", total_num_color_neighbors_found);
        printf("Fraction of points with color neighbours found : %f\n", (double)total_num_color_neighbors_found/indices.size());
      }
    }
    // if (env_params_.use_external_render == 1) {
    //   int num_color_neighbors_found =
    //       getNumColorNeighbours(point, indices, projected_cloud_);
    //
    //   printf("Color neighbours : %d\n", num_color_neighbors_found);
    //
    //   // if (num_color_neighbors_found < perch_params_.min_neighbor_points_for_valid_pose) {
    //   if (num_color_neighbors_found == 0) {
    //       return false;
    //   }
    // }
  }


  // TODO: revisit this and accomodate for collision model
  if (env_params_.use_external_pose_list != 1) {

    double rad_1, rad_2;
    rad_1 = obj_models_[model_id].GetInscribedRadius();

    for (size_t ii = 0; ii < s.NumObjects(); ++ii) {
      const auto object_state = s.object_states()[ii];
      int obj_id = object_state.id();
      ContPose obj_pose = object_state.cont_pose();

      rad_2 = obj_models_[obj_id].GetInscribedRadius();

      if ((pose.x() - obj_pose.x()) * (pose.x() - obj_pose.x()) +
          (pose.y() - obj_pose.y()) *
          (pose.y() - obj_pose.y()) < (rad_1 + rad_2) * (rad_1 + rad_2))  {
        // std::cout << "Invalid 2" << endl;
        return false;
      }
    }
  }

  // Do collision checking.
  // if (s.NumObjects() > 1) {
  //   printf("Model ids: %d %d\n", model_id, s.object_states().back().id());
  //   if (ObjectsCollide(obj_models_[model_id], obj_models_[s.object_states().back().id()], pose, s.object_states().back().cont_pose())) {
  //     return false;
  //   }
  // }

  if (env_params_.use_external_pose_list != 1) {
    // Check if the footprint is contained with the support surface bounds.
    auto footprint = obj_models_[model_id].GetFootprint(pose);

    for (const auto &point : footprint->points) {
      if (point.x < env_params_.x_min - perch_params_.footprint_tolerance ||
          point.x > env_params_.x_max + perch_params_.footprint_tolerance ||
          point.y < env_params_.y_min - perch_params_.footprint_tolerance ||
          point.y > env_params_.y_max + perch_params_.footprint_tolerance) {

        // printf("Bounds (x,y) : %f, %f, %f, %f\n", env_params_.x_min, env_params_.x_max, env_params_.y_min, env_params_.y_max);
        // std::cout << "Invalid 3" << endl;
        return false;
      }
    }
  }

  // Check if the footprint at this object pose contains at least one of the
  // constraint points. Is constraint_cloud is empty, then we will bypass this
  // check.
  if (!constraint_cloud_->empty()) {
    vector<bool> points_inside = obj_models_[model_id].PointsInsideFootprint(
                                   projected_constraint_cloud_, pose);
    int num_inside = 0;

    for (size_t ii = 0; ii < points_inside.size(); ++ii) {
      if (points_inside[ii]) {
        ++num_inside;
      }
    }

    // TODO: allow for more sophisticated validation? If not, then implement a
    // AnyPointInsideFootprint method to speed up checking.
    const double min_points = std::min(
                                perch_params_.min_points_for_constraint_cloud,
                                static_cast<int>(constraint_cloud_->points.size()));

    if (num_inside < min_points) {
      // std::cout << "Invalid 4" << endl;
      return false;
    }
  }

  return true;
}

void EnvObjectRecognition::LabelEuclideanClusters() {
  std::vector<PointCloudPtr> cluster_clouds;
  std::vector<pcl::PointIndices> cluster_indices;
  // An image where every pixel stores the cluster index.
  std::vector<int> cluster_labels;
  perception_utils::DoEuclideanClustering(observed_organized_cloud_,
                                          &cluster_clouds, &cluster_indices);
  cluster_labels.resize(observed_organized_cloud_->size(), 0);

  for (size_t ii = 0; ii < cluster_indices.size(); ++ii) {
    const auto &cluster = cluster_indices[ii];
    printf("PCD Dims: %d %d\n", observed_organized_cloud_->width,
           observed_organized_cloud_->height);

    for (const auto &index : cluster.indices) {
      int u = index % kCameraWidth;
      int v = index / kCameraWidth;
      int image_index = v * kCameraWidth + u;
      cluster_labels[image_index] = static_cast<int>(ii + 1);
    }
  }

  static cv::Mat image;
  image.create(kCameraHeight, kCameraWidth, CV_8UC1);

  for (int ii = 0; ii < kCameraHeight; ++ii) {
    for (int jj = 0; jj < kCameraWidth; ++jj) {
      int index = ii * kCameraWidth + jj;
      image.at<uchar>(ii, jj) = static_cast<uchar>(cluster_labels[index]);
    }
  }

  cv::normalize(image, image, 0, 255, cv::NORM_MINMAX, CV_8UC1);

  static cv::Mat c_image;
  cv::applyColorMap(image, c_image, cv::COLORMAP_JET);
  string fname = debug_dir_ + "cluster_labels.png";
  cv::imwrite(fname.c_str(), c_image);
}


bool compareCostComputationOutput(CostComputationOutput i1, CostComputationOutput i2)
{
    return (i1.cost < i2.cost);
}

void EnvObjectRecognition::GetSuccs(int source_state_id,
                                    vector<int> *succ_ids, vector<int> *costs) {

  printf("GetSuccs() for state\n");
  auto start = chrono::steady_clock::now();
  succ_ids->clear();
  costs->clear();

  if (source_state_id == env_params_.goal_state_id) {
    return;
  }

  GraphState source_state;

  if (adjusted_states_.find(source_state_id) != adjusted_states_.end()) {
    source_state = adjusted_states_[source_state_id];
  } else {
    source_state = hash_manager_.GetState(source_state_id);
  }

  // If in cache, return
  auto it = succ_cache.find(source_state_id);

  if (it != succ_cache.end()) {
    *costs = cost_cache[source_state_id];

    if (static_cast<int>(source_state.NumObjects()) == env_params_.num_objects -
        1) {
      succ_ids->resize(costs->size(), env_params_.goal_state_id);
    } else {
      *succ_ids = succ_cache[source_state_id];
    }

    printf("Expanding cached state: %d with %zu objects\n",
           source_state_id,
           source_state.NumObjects());
    return;
  }

  printf("Expanding state: %d with %zu objects\n",
         source_state_id,
         source_state.NumObjects());

  if (perch_params_.print_expanded_states) {
    string fname = debug_dir_ + "expansion_depth_" + to_string(source_state_id) + ".png";
    string cname = debug_dir_ + "expansion_color_" + to_string(source_state_id) + ".png";
    PrintState(source_state_id, fname, cname);
    // PrintState(source_state_id, fname);
  }

  vector<int> candidate_succ_ids, candidate_costs;
  vector<GraphState> candidate_succs;

  GenerateSuccessorStates(source_state, &candidate_succs);

  env_stats_.scenes_rendered += static_cast<int>(candidate_succs.size());

  // We don't need IDs for the candidate succs at all.
  candidate_succ_ids.resize(candidate_succs.size(), 0);

  vector<unsigned short> source_depth_image;
  vector<vector<unsigned char>> source_color_image;
  cv::Mat source_cv_depth_image;
  cv::Mat source_cv_color_image;

  // Commented by Aditya, do this in computecost only once to prevent multiple copies of the same source image vectors
  // GetDepthImage(source_state, &source_depth_image, &source_color_image,
  //               &source_cv_depth_image, &source_cv_color_image);

  candidate_costs.resize(candidate_succ_ids.size());

  // Prepare the cost computation input vector.
  vector<CostComputationInput> cost_computation_input(candidate_succ_ids.size());

  for (size_t ii = 0; ii < cost_computation_input.size(); ++ii) {
    auto &input_unit = cost_computation_input[ii];
    input_unit.source_state = source_state;
    input_unit.child_state = candidate_succs[ii];
    input_unit.source_id = source_state_id;
    input_unit.child_id = candidate_succ_ids[ii];
    input_unit.source_depth_image = source_depth_image;
    input_unit.source_color_image = source_color_image;
    input_unit.source_counted_pixels = counted_pixels_map_[source_state_id];
  }
  vector<CostComputationOutput> cost_computation_output;
  if (perch_params_.use_gpu)
  {
    // Use GPU in tree search - currently doesnt work
    ComputeCostsInParallelGPU(cost_computation_input, &cost_computation_output,
                          false);
  }
  else
  {
    ComputeCostsInParallel(cost_computation_input, &cost_computation_output,
                         false);
  }
  // hash_manager_.Print();

  // Sort in increasing order of cost for debugging
  // std::sort(cost_computation_output.begin(), cost_computation_output.end(), compareCostComputationOutput);
  // printf("candidate_succ_ids.size() %d\n", candidate_succ_ids.size());
  //---- PARALLELIZE THIS LOOP-----------//
  for (size_t ii = 0; ii < candidate_succ_ids.size(); ++ii) {
    const auto &output_unit = cost_computation_output[ii];
    // std::cout<<output_unit.cost;
    bool invalid_state = output_unit.cost == -1;

    // if (output_unit.cost != -1) {
    //   // Get the ID of the existing state, or create a new one if it doesn't
    //   // exist.
    //   int modified_state_id = hash_manager_.GetStateIDForceful(
    //                             output_unit.adjusted_state);
    //
    //   // If this successor exists and leads to a worse g-value, skip it.
    //   if (g_value_map_.find(modified_state_id) != g_value_map_.end() &&
    //       g_value_map_[modified_state_id] <= g_value_map_[source_state_id] +
    //       output_unit.cost) {
    //     invalid_state = true;
    //     // Otherwise, return the ID of the existing state and update its
    //     // continuous coordinates.
    //   } else {
    //     candidate_succ_ids[ii] = modified_state_id;
    //     hash_manager_.UpdateState(output_unit.adjusted_state);
    //   }
    // }

    const auto &input_unit = cost_computation_input[ii];
    candidate_succ_ids[ii] = hash_manager_.GetStateIDForceful(
                               input_unit.child_state);


    // if (env_params_.use_external_pose_list != 1)
    // {
    if (adjusted_states_.find(candidate_succ_ids[ii]) != adjusted_states_.end()) {
      // The candidate successor graph state should not exist in adjusted_states_
      // printf("Invalid state : %d\n", candidate_succ_ids[ii]);
      // invalid_state = true;
    }
    // }

    if (invalid_state) {
      candidate_costs[ii] = -1;
    } else {

      adjusted_states_[candidate_succ_ids[ii]] = output_unit.adjusted_state;
      assert(output_unit.depth_image.size() != 0);
      // if (env_params_.use_external_render == 1)
      // {
      //   assert(output_unit.color_image.size() != 0);
      // }
      candidate_costs[ii] = output_unit.cost;
      minz_map_[candidate_succ_ids[ii]] =
        output_unit.state_properties.last_min_depth;
      maxz_map_[candidate_succ_ids[ii]] =
        output_unit.state_properties.last_max_depth;
      counted_pixels_map_[candidate_succ_ids[ii]] = output_unit.child_counted_pixels;
      g_value_map_[candidate_succ_ids[ii]] = g_value_map_[source_state_id] +
                                             output_unit.cost;

      last_object_rendering_cost_[candidate_succ_ids[ii]] =
        output_unit.state_properties.target_cost +
        output_unit.state_properties.source_cost;

      // Cache the depth image only for single object renderings, *only* if valid.
      // NOTE: The hash key is computed on the *unadjusted* child state.
      if (source_state.NumObjects() == 0) {
        // Cache first successors
        // depth_image_cache_[candidate_succ_ids[ii]] = output_unit.depth_image;
        adjusted_single_object_depth_image_cache_[cost_computation_input[ii].child_state]
          =
            output_unit.depth_image;
        unadjusted_single_object_depth_image_cache_[cost_computation_input[ii].child_state]
          =
            output_unit.unadjusted_depth_image;

        // adjusted_single_object_color_image_cache_[cost_computation_input[ii].child_state]
        //   =
        //     output_unit.color_image;
        // unadjusted_single_object_color_image_cache_[cost_computation_input[ii].child_state]
        //   =
        //     output_unit.unadjusted_color_image;

        adjusted_single_object_state_cache_[cost_computation_input[ii].child_state] =
          output_unit.adjusted_state;

        if (kUseHistogramLazy)
        {
          adjusted_single_object_histogram_score_cache_[cost_computation_input[ii].child_state] =
            output_unit.histogram_score;
          printf("Caching histogram scores : %f\n",  output_unit.histogram_score);
        }

        assert(output_unit.adjusted_state.object_states().size() > 0);
        assert(adjusted_single_object_state_cache_[cost_computation_input[ii].child_state].object_states().size()
               > 0);
      }
    }
  }

  auto end = chrono::steady_clock::now();
  std::cout<< "real planning time: " << chrono::duration_cast<chrono::milliseconds>(end - start).count() << std::endl;
  //--------------------------------------//
  int min_cost = 9999999999;
  PointCloudPtr min_cost_point_cloud;
  printf("State number,     target_cost    source_cost    last_level_cost    candidate_costs    g_value_map\n");
  for (size_t ii = 0; ii < candidate_succ_ids.size(); ++ii) {
    const auto &output_unit = cost_computation_output[ii];

    if (candidate_costs[ii] == -1 || candidate_costs[ii] == -2) {
      continue;  // Invalid successor
    }


    if (IsGoalState(candidate_succs[ii])) {
      succ_ids->push_back(env_params_.goal_state_id);
    } else {
      succ_ids->push_back(candidate_succ_ids[ii]);
    }

    succ_cache[source_state_id].push_back(candidate_succ_ids[ii]);
    costs->push_back(candidate_costs[ii]);
    // image_debug_=false;
    if (image_debug_) {
      // std::stringstream ss;
      // ss.precision(20);
      // ss << debug_dir_ + "succ_depth_" << candidate_succ_ids[ii] << ".png";
      // std::stringstream ssc;
      // ssc.precision(20);
      // ssc << debug_dir_ + "succ_color_" << candidate_succ_ids[ii] << ".png";
      // // PrintImage(ss.str(), output_unit.depth_image);
      // PrintState(output_unit.adjusted_state , ss.str(), ssc.str());

      // uint8_t rgb[3] = {255,0,0};
      // // auto gravity_aligned_point_cloud = GetGravityAlignedPointCloud(
      // //                                      output_unit.depth_image, output_unit.color_image);
      // auto gravity_aligned_point_cloud = GetGravityAlignedPointCloud(
      //     output_unit.depth_image, rgb);
      // PrintPointCloud(gravity_aligned_point_cloud, candidate_succ_ids[ii], render_point_cloud_topic);

      printf("State %d,       %d      %d      %d      %d      %d\n",
             candidate_succ_ids[ii],
             output_unit.state_properties.target_cost,
             output_unit.state_properties.source_cost,
             output_unit.state_properties.last_level_cost,
             candidate_costs[ii],
             g_value_map_[candidate_succ_ids[ii]]);

      // if (candidate_costs[ii] < min_cost)
      // {
      //    min_cost = candidate_costs[ii];
      //    rgb[0] = 0;
      //    rgb[1] = 255;
      //    rgb[2] = 0;
      //   //  min_cost_point_cloud = GetGravityAlignedPointCloud(
      //   //                                       output_unit.depth_image, output_unit.color_image);
      //    min_cost_point_cloud = GetGravityAlignedPointCloud(
      //                                         output_unit.depth_image, rgb);
      // }
      // std::stringstream cloud_ss;
      // cloud_ss.precision(20);
      // cloud_ss << debug_dir_ + "cloud_" << candidate_succ_ids[ii] << ".pcd";
      // pcl::PCDWriter writer;
      // writer.writeBinary (cloud_ss.str()  , *gravity_aligned_point_cloud);
      //
      // sensor_msgs::PointCloud2 output;
      // pcl::PCLPointCloud2 outputPCL;
      // pcl::toPCLPointCloud2( *gravity_aligned_point_cloud ,outputPCL);
      //
      // // Convert to ROS data type
      // pcl_conversions::fromPCL(outputPCL, output);
      // output.header.frame_id = "base_footprint";
      //
      // render_point_cloud_topic.publish(output);
      // ros::spinOnce();
    }
  }

  if (image_debug_) {
      printf("Point cloud with least cost has cost value : %f\n", min_cost);
      // PrintPointCloud(min_cost_point_cloud, -1);
  }

  // cache succs and costs
  cost_cache[source_state_id] = *costs;

  if (perch_params_.debug_verbose) {
    printf("Succs for %d\n", source_state_id);

    for (int ii = 0; ii < static_cast<int>(succ_ids->size()); ++ii) {
      printf("%d  ,  %d\n", (*succ_ids)[ii], (*costs)[ii]);
    }

    printf("\n");
  }

  // ROS_INFO("Expanding state: %d with %d objects and %d successors",
  //          source_state_id,
  //          source_state.object_ids.size(), costs->size());
  // string fname = debug_dir_ + "expansion_" + to_string(source_state_id) + ".png";
  // PrintState(source_state_id, fname);
}

void EnvObjectRecognition::PrintPointCloud(PointCloudPtr gravity_aligned_point_cloud, int state_id, ros::Publisher point_cloud_topic)
{
    // printf("Print File Cloud Size : %d\n", gravity_aligned_point_cloud->points.size());
    std::stringstream cloud_ss;
    cloud_ss.precision(20);
    cloud_ss << debug_dir_ + "cloud_" << state_id << ".pcd";
    pcl::PCDWriter writer;
    // writer.writeBinary (cloud_ss.str(), *gravity_aligned_point_cloud);

    sensor_msgs::PointCloud2 output;

    pcl::PCLPointCloud2 outputPCL;
    pcl::toPCLPointCloud2( *gravity_aligned_point_cloud ,outputPCL);

    // Convert to ROS data type
    // pcl::TextureMesh meshT;
    // pcl::io::loadPolygonFileOBJ("/media/aditya/A69AFABA9AFA85D9/Cruzr/code/DOPE/catkin_ws/src/perception/sbpl_perception/data/YCB_Video_Dataset/models/004_sugar_box/textured.obj", meshT);

    // printf("Obj File Cloud Size : %d\n", meshT.cloud.data.size());

    pcl_conversions::fromPCL(outputPCL, output);
    output.header.frame_id = env_params_.reference_frame_;

    point_cloud_topic.publish(output);
    // ros::spinOnce();
    // ros::Rate loop_rate(5);
    // loop_rate.sleep();
}
int EnvObjectRecognition::GetBestSuccessorID(int state_id) {
  printf("GetBestSuccessorID() called\n");
  const auto &succ_costs = cost_cache[state_id];
  assert(!succ_costs.empty());
  const auto min_element_it = std::min_element(succ_costs.begin(),
                                               succ_costs.end());
  int offset = std::distance(succ_costs.begin(), min_element_it);
  const auto &succs = succ_cache[state_id];
  int best_succ_id = succs[offset];
  printf("GetBestSuccessorID() done, best_succ_id : %d\n", best_succ_id);
  return best_succ_id;
}


void EnvObjectRecognition::ComputeCostsInParallel(std::vector<CostComputationInput> &input,
                                                  std::vector<CostComputationOutput> *output,
                                                  bool lazy) {
  std::cout << "Computing costs in parallel" << endl;
  int count = 0;
  int original_count = 0;
  // auto appended_input = input;
  const int num_processors = static_cast<int>(mpi_comm_->size());
  if (cost_debug_msgs)
    printf("num_processors : %d\n", num_processors);

  if (mpi_comm_->rank() == kMasterRank) {
    original_count = count = input.size();

    if (count % num_processors != 0) {
      if (cost_debug_msgs)
        printf("resizing input\n");
      count += num_processors - count % num_processors;
      CostComputationInput dummy_input;
      dummy_input.source_id = -1;
      // add dummy inputs to input vector to make it same on all processors
      input.resize(count, dummy_input);
    }

    assert(output != nullptr);
    output->clear();
    output->resize(count);
  }

  broadcast(*mpi_comm_, count, kMasterRank);
  broadcast(*mpi_comm_, lazy, kMasterRank);

  if (count == 0) {
    return;
  }

  int recvcount = count / num_processors;

  std::vector<CostComputationInput> input_partition(recvcount);
  std::vector<CostComputationOutput> output_partition(recvcount);
  boost::mpi::scatter(*mpi_comm_, input, &input_partition[0], recvcount,
                      kMasterRank);

  vector<unsigned short> source_depth_image;
  vector<vector<unsigned char>> source_color_image;
  cv::Mat source_cv_depth_image;
  cv::Mat source_cv_color_image;

  GetDepthImage(input_partition[0].source_state,
                &source_depth_image, &source_color_image,
                &source_cv_depth_image, &source_cv_color_image);

  // printf("recvcount : %d\n", recvcount);
  for (int ii = 0; ii < recvcount; ++ii) {
    if (cost_debug_msgs)
      printf("State number being processed : %d\n", ii);
    const auto &input_unit = input_partition[ii];
    auto &output_unit = output_partition[ii];

    // If this is a dummy input, skip computation.
    if (input_unit.source_id == -1) {
      output_unit.cost = -1;
      continue;
    }

    if (!lazy) {
      output_unit.cost = GetCost(input_unit.source_state, input_unit.child_state,
                                 source_depth_image,
                                 source_color_image,
                                 input_unit.source_counted_pixels,
                                 &output_unit.child_counted_pixels, &output_unit.adjusted_state,
                                 &output_unit.state_properties, &output_unit.depth_image,
                                 &output_unit.color_image,
                                 &output_unit.unadjusted_depth_image,
                                 &output_unit.unadjusted_color_image,
                                 output_unit.histogram_score);
    } else {
      if (input_unit.unadjusted_last_object_depth_image.empty()) {
        output_unit.cost = -1;
      } else {
        output_unit.cost = GetLazyCost(input_unit.source_state, input_unit.child_state,
                                       source_depth_image,
                                       source_color_image,
                                       input_unit.unadjusted_last_object_depth_image,
                                       input_unit.adjusted_last_object_depth_image,
                                       input_unit.adjusted_last_object_state,
                                       input_unit.source_counted_pixels,
                                       input_unit.adjusted_last_object_histogram_score,
                                       &output_unit.adjusted_state,
                                       &output_unit.state_properties,
                                       &output_unit.depth_image);
      }
    }
    // input_unit.source_depth_image.clear();
    // input_unit.source_depth_image.shrink_to_fit();
  }

  boost::mpi::gather(*mpi_comm_, &output_partition[0], recvcount, *output,
                     kMasterRank);

  if (mpi_comm_->rank() == kMasterRank) {
    output->resize(original_count);
  }
}

void EnvObjectRecognition::PrintGPUImages(std::vector<int32_t>& result_depth, 
                                      std::vector<std::vector<uint8_t>>& result_color, 
                                      int num_poses, string suffix, 
                                      vector<int> poses_occluded,
                                      const vector<int> cost)
{
  //// Function to check if the GPU rendered images looks fine by converting them to point cloud on CPU
  //// and visualizing the output

  // for (int j = 0; j < result_depth.size(); j++) {
  //   if (result_depth[j] > 0)
  //   {
  //     // cout << result_depth[j]/100.0 << endl;
  //   }
  // }
  #pragma omp parallel for
  for(int n = 0; n < num_poses; n ++){
      cv::Mat cv_color = cv::Mat(env_params_.height,env_params_.width,CV_8UC3);
      cv::Mat cv_depth = cv::Mat(env_params_.height,env_params_.width,CV_32SC1);//, result_depth.data() + n*env_params_.height*env_params_.width);
      if (poses_occluded[n]) continue;
      for(int i = 0; i < env_params_.height; i ++){
          for(int j = 0; j < env_params_.width; j ++){
              int index = n*env_params_.width*env_params_.height+(i*env_params_.width+j);
              int red = result_color[0][index];
              int green = result_color[1][index];
              int blue = result_color[2][index];
              // std::cout<<red<<","<<green<<","<<blue<<std::endl;
              cv_color.at<cv::Vec3b>(i, j) = cv::Vec3b(blue, green,red);
              cv_depth.at<int32_t>(i, j) = result_depth[index];
              // if (result_depth[index] > 0)
                // printf("%d,%d:%d\n",i,j,result_depth[index]);
                // printf("%d,%d:%d\n",i,j,cv_depth.at<int>(i, j));
          }
      }
      std::string color_image_path, depth_image_path;
      std::stringstream ss1, ss2;
      ss1 << debug_dir_ << "/gpu-" << suffix << "-" << n << "-color.png";
      color_image_path = ss1.str();
      ss2 << debug_dir_ << "/gpu-" << suffix << "-" << n << "-depth.png";
      depth_image_path = ss2.str();

      // cv::Mat color_depth_image;
      // ColorizeDepthImage(cv_depth, color_depth_image, min_observed_depth_, max_observed_depth_);
      if ((cost.size() > 0 && cost[n] < 200) || cost.size() == 0)
      {
        cv::imwrite(color_image_path, cv_color);
        // cv::imwrite(depth_image_path, cv_depth);
        // PointCloudPtr depth_img_cloud = 
        //   GetGravityAlignedPointCloudCV(cv_depth, cv_color, 100.0);
        // PrintPointCloud(depth_img_cloud, 1, render_point_cloud_topic);
      }
      
      // cv::imwrite(depth_image_path, color_depth_image);

      // double min;
      // double max;
      // cv::minMaxIdx(cv_depth, &min, &max);
      // cv::Mat adjMap;
      // cv_depth.convertTo(adjMap,CV_8UC1, 255 / (max-min), -min);
      // cv::Mat falseColorsMap;
      // applyColorMap(adjMap, falseColorsMap, cv::COLORMAP_HOT);

      
      // std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  
}
void EnvObjectRecognition::GetICPAdjustedPosesCPU(const vector<ObjectState>& objects,
                                                  int num_poses,
                                                  float* result_cloud,
                                                  uint8_t* result_cloud_color,
                                                  int rendered_point_num,
                                                  int* cloud_pose_map,
                                                  int* pose_occluded,
                                                  vector<ObjectState>& modified_objects,
                                                  bool do_icp,
                                                  ros::Publisher render_point_cloud_topic,
                                                  bool print_cloud)
{
  /*
   * Does CPU ICPs on all point clouds coming as output of GPU rendering for 3-Dof
   * Can also do GPU fast_gicp but point clouds will be copied to GPU
   * do_icp and pose_occluded dont serve any purpose at the moment
   */
  chrono::time_point<chrono::system_clock> start, end;
  start = chrono::system_clock::now();
  if (objects.size() > 0)
  {
    ObjectState temp;
    modified_objects.resize(objects.size(), temp);
  }
  Eigen::Isometry3d transform;
  Eigen::Isometry3d cam_to_body;
  cam_to_body.matrix() << 0, 0, 1, 0,
                    -1, 0, 0, 0,
                    0, -1, 0, 0,
                    0, 0, 0, 1;
  transform = cam_to_world_ * cam_to_body;
  printf("GetICPAdjustedPosesCPU()\n");

  if (perch_params_.icp_type == 2)
  {
    // GPU GICP , used for 6-Dof
    // Avoid using this, unless debugging GPU GICP
    vector<int> pose_segmen_label;
    for (int i = 0; i < num_poses; i++)
    {
      // Need to do -1 no 0s in segmentation label for segmented nearest neighbour search
      pose_segmen_label.push_back(objects[i].segmentation_label_id() - 1);
    }
    // Testing CUDA GICP
    fast_gicp::FastGICPCuda<pcl::PointXYZ, pcl::PointXYZ> gicp_cuda;
    std::vector<Eigen::Isometry3f> estimated;
    gicp_cuda.setMaximumIterations(perch_params_.max_icp_iterations);
    gicp_cuda.setCorrespondenceRandomness(10);
    gicp_cuda.computeTransformationMulti(result_cloud,
                                          rendered_point_num,
                                          result_observed_cloud,
                                          observed_point_num,
                                          cloud_pose_map,
                                          result_observed_cloud_label,
                                          pose_segmen_label.data(),
                                          num_poses,
                                          estimated);
    for (int n = 0; n < num_poses; n++)
    {
      ContPose pose_in = objects[n].cont_pose();
      ContPose pose_out;
      Eigen::Matrix4f transformation;
      transformation = estimated[n].matrix();

      Eigen::Matrix4f transformation_old = pose_in.GetTransform().matrix().cast<float>();
      Eigen::Matrix4f transformation_new = transformation * transformation_old;
      Eigen::Vector4f vec_out;
      if (env_params_.use_external_pose_list == 0)
      {
        Eigen::Vector4f vec_in;
        vec_in << pose_in.x(), pose_in.y(), pose_in.z(), 1.0;
        vec_out = transformation * vec_in;
        double yaw = atan2(transformation(1, 0), transformation(0, 0));

        double yaw1 = pose_in.yaw();
        double yaw2 = yaw;
        double cos_term = cos(yaw1 + yaw2);
        double sin_term = sin(yaw1 + yaw2);
        double total_yaw = atan2(sin_term, cos_term);
        pose_out = ContPose(vec_out[0], vec_out[1], vec_out[2], pose_in.roll(), pose_in.pitch(), total_yaw);
      }
      else
      {
        vec_out << transformation_new(0, 3), transformation_new(1, 3), transformation_new(2, 3), 1.0;
        Matrix3f rotation_new(3, 3);
        for (int i = 0; i < 3; i++)
        {
          for (int j = 0; j < 3; j++)
          {
            rotation_new(i, j) = transformation_new(i, j);
          }
        }
        auto euler = rotation_new.eulerAngles(2, 1, 0);
        double roll_ = euler[0];
        double pitch_ = euler[1];
        double yaw_ = euler[2];

        Quaternionf quaternion(rotation_new.cast<float>());

        pose_out = ContPose(vec_out[0], vec_out[1], vec_out[2], quaternion.x(), quaternion.y(), quaternion.z(), quaternion.w());
      }
      ObjectState modified_object_state(objects[n].id(),
                                        objects[n].symmetric(), pose_out, objects[n].segmentation_label_id());
      modified_objects[n] = modified_object_state;
    }
  }
  else if (perch_params_.icp_type == 0 || perch_params_.icp_type == 1)
  {
    vector<int> parent_counted_pixels;
    uint8_t rgb[3] = {0,0,0};

    vector<PointCloudPtr> cloud;
    for (int i = 0; i < num_poses; i++)
    {
      PointCloudPtr empty(new PointCloud);
      cloud.push_back(empty);
    }

    // Create separate PCL clouds from point cloud array from GPU
    for(int cloud_index = 0; cloud_index < rendered_point_num; cloud_index = cloud_index + 1)
    {
      // Get the pose index
      int n = cloud_pose_map[cloud_index];
      if ((do_icp && pose_occluded[n]) || (!do_icp && print_cloud))
      {

        pcl::PointXYZRGB point;
        point.x = result_cloud[cloud_index + 0*rendered_point_num];
        point.y = result_cloud[cloud_index + 1*rendered_point_num];
        point.z = result_cloud[cloud_index + 2*rendered_point_num];
        rgb[2] = result_cloud_color[cloud_index + 0*rendered_point_num];
        rgb[1] = result_cloud_color[cloud_index + 1*rendered_point_num];
        rgb[0] = result_cloud_color[cloud_index + 2*rendered_point_num];
        uint32_t rgbc = ((uint32_t)rgb[2] << 16 | (uint32_t)rgb[1]<< 8 | (uint32_t)rgb[0]);
        point.rgb = *reinterpret_cast<float*>(&rgbc);

        cloud[n]->points.push_back(point);
      }
    }

    // ICP on parallel GPU threads
    #pragma omp parallel for if (do_icp)
    for (int n = 0; n < num_poses; n++)
    {
      if (cost_debug_msgs)
        printf("ICP for Pose index : %d\n", n);
      PointCloudPtr cloud_in = cloud[n];
      cloud_in->width = 1;
      cloud_in->height = cloud_in->points.size();
      cloud_in->is_dense = false;

      PointCloudPtr transformed_cloud(new PointCloud);
      PointCloudPtr cloud_out(new PointCloud);
      if (env_params_.use_external_pose_list == 1)
      {
        *transformed_cloud = *cloud_in;
      }
      else
      {
        transformPointCloud (*cloud_in, *transformed_cloud, transform.matrix().cast<float>());
      }
      
      if (print_cloud)
      {
        PrintPointCloud(transformed_cloud, 1, render_point_cloud_topic);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }
      
      if (do_icp)
      {
        if(pose_occluded[n])
        {
          ContPose pose_in = objects[n].cont_pose();
          ContPose pose_out;
          if (env_params_.use_external_pose_list == 1)
          {
            // For 6-Dof do CPU GICP
            string model_name = obj_models_[objects[n].id()].name();
            int required_object_id = distance(segmented_object_names.begin(), 
            find(segmented_object_names.begin(), segmented_object_names.end(), model_name));
            // GetICPAdjustedPose(
            GetVGICPAdjustedPose(
              transformed_cloud, pose_in, cloud_out, &pose_out, parent_counted_pixels, 
              segmented_object_clouds[required_object_id], model_name);
          }
          else
          {
            // Use for 3-Dof, PCL ICP
            GetICPAdjustedPose(
              transformed_cloud, pose_in, cloud_out, &pose_out, parent_counted_pixels);
          }
          // cout << "pose_in " << pose_in << endl;
          // cout << "pose_out " << pose_out << endl;
          if (print_cloud)
          {
            PrintPointCloud(cloud_out, 1, render_point_cloud_topic);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
          }
          ObjectState modified_object_state(objects[n].id(),
                                                objects[n].symmetric(), pose_out, objects[n].segmentation_label_id());
          modified_objects[n] = modified_object_state;
        }
        else
        {
          modified_objects[n] = objects[n];
        }
      }
    }
  }
  end = chrono::system_clock::now();
  chrono::duration<double> elapsed_seconds = end-start;
  env_stats_.icp_time += elapsed_seconds.count();
  std::cout << "GetICPAdjustedPosesCPU() took "
          << elapsed_seconds.count()
          << " milliseconds\n";
}
void EnvObjectRecognition::PrintGPUClouds(const vector<ObjectState>& objects,
                                          float* result_cloud, 
                                          uint8_t* result_cloud_color,
                                          int* result_depth, 
                                          int* dc_index, 
                                          int num_poses, 
                                          int cloud_point_num, 
                                          int stride,
                                          int* pose_occluded,
                                          string suffix,
                                          vector<ObjectState>& modified_objects,
                                          bool do_icp,
                                          ros::Publisher render_point_cloud_topic,
                                          bool print_cloud)
{ 
  // ofstream myfile;
  //// Function to check if the GPU point cloud looks fine, and also do ICP if needed
  vector<int> parent_counted_pixels;

  uint8_t rgb[3] = {0,0,0};
  Eigen::Isometry3d transform;
  Eigen::Isometry3d cam_to_body;
  cam_to_body.matrix() << 0, 0, 1, 0,
                    -1, 0, 0, 0,
                    0, -1, 0, 0,
                    0, 0, 0, 1;
  transform = cam_to_world_ * cam_to_body;
  if (objects.size() > 0)
  {
    ObjectState temp;
    modified_objects.resize(objects.size(), temp);
  }
  // #pragma omp parallel for if (do_icp)
  for(int n = 0; n < num_poses; n ++)
  {
    // if(pose_occluded[n]) continue;
    
    PointCloudPtr cloud(new PointCloud);
    PointCloudPtr transformed_cloud(new PointCloud);
    PointCloudPtr cloud_out(new PointCloud);

    // myfile.open(suffix + "_" + to_string(n) + ".txt");
    if ((do_icp && pose_occluded[n]) || (!do_icp && print_cloud))
    {

      for(int i = 0; i < env_params_.height; i = i + stride)
      {
          for(int j = 0; j < env_params_.width; j = j + stride)
          {
            pcl::PointXYZRGB point;
            int index = n*env_params_.width*env_params_.height + (i*env_params_.width + j);
            int cloud_index = dc_index[index];
            // printf("for image:(%d,%d,%d), cloud_index:%d\n", n,i,j, cloud_index);
            if (result_depth[index] > 0)
            {
              // printf("x:%f,y:%f,z:%f\n", result_cloud[3*index], result_cloud[3*index + 1], result_cloud[3*index + 2]);
              point.x = result_cloud[cloud_index + 0*cloud_point_num];
              point.y = result_cloud[cloud_index + 1*cloud_point_num];
              point.z = result_cloud[cloud_index + 2*cloud_point_num];
              rgb[2] = result_cloud_color[cloud_index + 0*cloud_point_num];
              rgb[1] = result_cloud_color[cloud_index + 1*cloud_point_num];
              rgb[0] = result_cloud_color[cloud_index + 2*cloud_point_num];
              // uint32_t rgbc = ((uint32_t)rgb[2] << 16 | (uint32_t)rgb[1]<< 8 | (uint32_t)rgb[0]);
              uint32_t rgbc = ((uint32_t)rgb[2] << 16 | (uint32_t)rgb[1]<< 8 | (uint32_t)rgb[0]);
              point.rgb = *reinterpret_cast<float*>(&rgbc);

              cloud->points.push_back(point);
              // myfile << point.x << "," << point.y << "," << point.z << endl;
            }
          }
      }
      cloud->width = 1;
      cloud->height = cloud->points.size();
      cloud->is_dense = false;
      transformPointCloud (*cloud, *transformed_cloud, transform.matrix().cast<float>());
      // *transformed_cloud = *cloud;
      if (print_cloud)
      {
        PrintPointCloud(transformed_cloud, 1, render_point_cloud_topic);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }
    }
  }
}
// void EnvObjectRecognition::GetICPAdjustedPosesGPU(float* result_rendered_clouds,
//                                                   int* dc_index,
//                                                   int32_t* depth_data,
//                                                   int num_poses,
//                                                   float* result_observed_cloud,
//                                                   int* observed_dc_index,
//                                                   int total_rendered_point_num,
//                                                   int* poses_occluded)
// {
//   // int num_points_observed = observed_dc_index[env_params_.width * env_params_.height-1];  
//   // // #pragma omp parallel for
//   // for (int i = 0; i < num_poses-1; i++)
//   // {
//   //   int n = i;
//   //   int begin_points_curr_pose = dc_index[n*env_params_.width * env_params_.height];
//   //   int end_points_curr_pose = dc_index[(n+1)*env_params_.width * env_params_.height];
//   //   int num_points_curr_pose = end_points_curr_pose - begin_points_curr_pose;
//   //   printf("Number of target points (c++): %d\n", num_points_curr_pose);

//   //   ICP* icp_gpu = new ICP(result_observed_cloud, 
//   //                         num_points_observed,
//   //                         result_rendered_clouds,
//   //                         begin_points_curr_pose,
//   //                         end_points_curr_pose,
//   //                         total_rendered_point_num);

//   //   float* rendered_icp_adjusted_cloud = (float*) malloc(3 * num_points_curr_pose * sizeof(float));
//   //   icp_gpu->iterateGPU(rendered_icp_adjusted_cloud);
//   //   // icp_gpu->endSimulation();
//   //   // PrintGPUClouds(rendered_icp_adjusted_cloud, depth_data, dc_index, 1, num_points_curr_pose, 1, poses_occluded, "icp");
//   //   free(rendered_icp_adjusted_cloud);
//   //   free(icp_gpu);
//   // }
// }
// Depracated : Use GetStateImagesUnifiedGPU
// void EnvObjectRecognition::GetStateImagesGPU(const vector<ObjectState>& objects,
//                                           const vector<vector<uint8_t>>& source_result_color,
//                                           const vector<int32_t>& source_result_depth,
//                                           vector<vector<uint8_t>>& result_color,
//                                           vector<int32_t>& result_depth,
//                                           vector<int>& pose_occluded,
//                                           int single_result_image,
//                                           vector<int>& pose_occluded_other,
//                                           vector<float>& pose_clutter_cost,
//                                           const vector<int>& pose_segmentation_label)
// {
//   /*
//     Takes a bunch of ObjectState objects and renders them. 
//     Possible outputs - 1 image output with multiple objects in different pose each in single image
//                      - Multiple images with different object in different poses, one object per image
//   */
//   printf("GetStateImagesGPU() for %d poses\n", objects.size());
//   Eigen::Isometry3d cam_z_front, cam_to_body;
//   cam_to_body.matrix() << 0, 0, 1, 0,
//                     -1, 0, 0, 0,
//                     0, -1, 0, 0,
//                     0, 0, 0, 1;
//   cam_z_front = cam_to_world_ * cam_to_body;
//   Eigen::Matrix4d cam_matrix =cam_z_front.matrix().inverse();

//   vector<cuda_renderer::Model::mat4x4> mat4_v;
//   vector<int> pose_model_map;
//   // vector<int> tris_model_count;
//   // vector<cuda_renderer::Model::Triangle> tris;

//   int num_poses = objects.size();
//   int model_id_prev = -1;

//   // Transform from table frame to camera frame
//   for(int i = 0; i < num_poses; i ++) {
//     int model_id = objects[i].id();

//     ContPose cur = objects[i].cont_pose();
//     Eigen::Matrix4d preprocess_transform = 
//       obj_models_[model_id].preprocessing_transform().matrix().cast<double>();
//     Eigen::Matrix4d transform;
//     transform = cur.ContPose::GetTransform().matrix().cast<double>();

//     // For non-6d dof this is needed to align model base to table, 
//     // we need to remove it because we are going back to camera frame on gpu render
//     // if (env_params_.use_external_pose_list != 1)
//     transform = transform * preprocess_transform;
    
//     Eigen::Matrix4d pose_in_cam = cam_matrix * transform;
//     cuda_renderer::Model::mat4x4 mat4;
//     mat4.init_from_eigen(pose_in_cam, 100);
//     // mat4.print();
//     mat4_v.push_back(mat4);

//     pose_model_map.push_back(model_id);
//     model_id_prev = model_id;
//   }
//   // std::vector<int> pose_occluded;
//   auto result_dev = cuda_renderer::render_cuda_multi(
//                           tris,
//                           mat4_v,
//                           pose_model_map,
//                           tris_model_count,
//                           env_params_.width, env_params_.height, 
//                           env_params_.proj_mat, 
//                           source_result_depth,
//                           source_result_color,
//                           result_depth, 
//                           result_color,
//                           pose_occluded,
//                           single_result_image,
//                           pose_occluded_other,
//                           pose_clutter_cost,
//                           predicted_mask_image,
//                           pose_segmentation_label);
// }


void EnvObjectRecognition::GetStateImagesUnifiedGPU(const string stage,
                    const vector<ObjectState>& objects,
                    const vector<vector<uint8_t>>& source_result_color,
                    const vector<int32_t>& source_result_depth,
                    vector<vector<uint8_t>>& result_color,
                    vector<int32_t>& result_depth,
                    int single_result_image,
                    vector<float>& pose_clutter_cost,
                    float* &result_cloud,
                    uint8_t* &result_cloud_color,
                    int& result_cloud_point_num,
                    int* &result_dc_index,
                    int* &result_cloud_pose_map,
                    vector<cuda_renderer::Model::mat4x4>& adjusted_poses,
                    float* &rendered_cost,
                    float* &observed_cost,
                    float* &points_diff_cost,
                    float sensor_resolution,
                    bool do_gpu_icp,
                    int cost_type,
                    bool calculate_observed_cost)
{
   /*
    Takes a bunch of ObjectState objects containing object ID and pose information and renders them. 
    Possible outputs - 1 image output with multiple objects in different pose each in single image
                     - Multiple images with different object in different poses, one object per image
                     - Rendered point clouds from the GPU
                     - Final costs from GPU with adjusted ICP poses
  */
  printf("GetStateImagesUnifiedGPU() for %d poses\n", objects.size());
  Eigen::Isometry3d cam_z_front, cam_to_body;
  cam_to_body.matrix() << 0, 0, 1, 0,
                    -1, 0, 0, 0,
                    0, -1, 0, 0,
                    0, 0, 0, 1;
  cam_z_front = cam_to_world_ * cam_to_body;
  Eigen::Matrix4d cam_matrix =cam_z_front.matrix().inverse();

  vector<cuda_renderer::Model::mat4x4> mat4_v;
  vector<int> pose_model_map;
  vector<int> pose_segmen_label;
  vector<float> pose_obs_points_total;

  // vector<int> tris_model_count;
  // vector<cuda_renderer::Model::Triangle> tris;

  int num_poses = objects.size();
  int model_id_prev = -1;

  // Transform from table frame to camera frame
  for(int i = 0; i < num_poses; i ++) {
    int model_id = objects[i].id();

    ContPose cur = objects[i].cont_pose();
    Eigen::Matrix4d preprocess_transform = 
      obj_models_[model_id].preprocessing_transform().matrix().cast<double>();
    Eigen::Matrix4d transform;
    transform = cur.ContPose::GetTransform().matrix().cast<double>();

    // For non-6d dof this is needed to align model base to table, 
    // we need to remove it because we are going back to camera frame on gpu render
    // if (env_params_.use_external_pose_list != 1)
    transform = transform * preprocess_transform;

    // cout << preprocess_transform << endl;
    
    Eigen::Matrix4d pose_in_cam = cam_matrix * transform;
    cuda_renderer::Model::mat4x4 mat4;
    mat4.init_from_eigen(pose_in_cam, 100);
    // mat4.print();
    mat4_v.push_back(mat4);

    pose_model_map.push_back(model_id);
    model_id_prev = model_id;

    if (env_params_.use_external_pose_list == 1)
    {
      int required_object_id = objects[i].segmentation_label_id() - 1; 

      // The segmentation label for the pose that identifies the object in the mask for 6dof
      // It should start from 0 from segmented kNN etc.
      pose_segmen_label.push_back(required_object_id);

      // Number of total points calculated from the mask, used for calculating observed cost
      pose_obs_points_total.push_back(segmented_observed_point_count[required_object_id]);
    }
    else 
    {
      if (perch_params_.use_cylinder_observed)
      {
        // Using points in pose cylinder volume for observed cost calculation
        PointT obj_center;
        obj_center.x = cur.x();
        obj_center.y = cur.y();
        obj_center.z = cur.z();
        vector<float> sqr_dists;
        vector<int> validation_points;

        // Get number of points in radius of pose where radius is cylinder circumscribing the object model
        const double validation_search_rad =
          obj_models_[model_id].GetInflationFactor() *
          obj_models_[model_id].GetCircumscribedRadius();
        int num_validation_neighbors = projected_knn_->radiusSearch(obj_center,
                                                    validation_search_rad,
                                                    validation_points,
                                                    sqr_dists, kNumPixels);

        // printf("validation_search_rad:%f, num_validation_neighbors:%d\n", validation_search_rad, num_validation_neighbors);
        pose_obs_points_total.push_back(num_validation_neighbors);
      }
      else
      {
        // Using all points in scene for observed cost calculation
        pose_obs_points_total.push_back(observed_point_num);
      }

    }
  }
  double peak_memory_usage;
  // Old function that doesnt use .cuh files to separate render, cloud generator etc.
  // cuda_renderer::render_cuda_multi_unified_old(
  //                         stage,
  //                         tris,
  //                         mat4_v,
  //                         pose_model_map,
  //                         tris_model_count,
  //                         env_params_.width, env_params_.height, 
  //                         env_params_.proj_mat, 
  //                         source_result_depth,
  //                         source_result_color,
  //                         single_result_image,
  //                         pose_clutter_cost,
  //                         predicted_mask_image,
  //                         pose_segmen_label,
  //                         gpu_stride,
  //                         gpu_point_dim,
  //                         gpu_depth_factor,
  //                         kCameraCX,
  //                         kCameraCY,
  //                         kCameraFX,
  //                         kCameraFY,
  //                         result_observed_cloud,
  //                         result_observed_cloud_color,
  //                         observed_point_num,
  //                         pose_obs_points_total,
  //                         result_observed_cloud_label,
  //                         cost_type,
  //                         calculate_observed_cost,
  //                         sensor_resolution,
  //                         perch_params_.color_distance_threshold,
  //                         perch_params_.gpu_occlusion_threshold,
  //                         result_depth,
  //                         result_color,
  //                         result_cloud,
  //                         result_cloud_color,
  //                         result_cloud_point_num,
  //                         result_cloud_pose_map,
  //                         result_dc_index,
  //                         rendered_cost,
  //                         observed_cost,
  //                         points_diff_cost,
  //                         peak_memory_usage);
  cuda_renderer::gpu_stats stats;

  // Get outputs from CUDA
  cuda_renderer::render_cuda_multi_unified(
                          stage,
                          tris,
                          mat4_v,
                          pose_model_map,
                          tris_model_count,
                          env_params_.width, env_params_.height, 
                          env_params_.proj_mat, 
                          source_result_depth,
                          source_result_color,
                          single_result_image,
                          pose_clutter_cost,
                          predicted_mask_image,
                          pose_segmen_label,
                          gpu_stride,
                          gpu_point_dim,
                          gpu_depth_factor,
                          kCameraCX,
                          kCameraCY,
                          kCameraFX,
                          kCameraFY,
                          result_observed_cloud,
                          result_observed_cloud_eigen,
                          result_observed_cloud_color,
                          observed_point_num,
                          pose_obs_points_total,
                          result_observed_cloud_label,
                          cost_type,
                          calculate_observed_cost,
                          sensor_resolution,
                          perch_params_.color_distance_threshold,
                          perch_params_.gpu_occlusion_threshold,
                          do_gpu_icp,
                          result_depth,
                          result_color,
                          result_cloud,
                          result_cloud_color,
                          result_cloud_point_num,
                          result_cloud_pose_map,
                          result_dc_index,
                          adjusted_poses,
                          rendered_cost,
                          observed_cost,
                          points_diff_cost,
                          stats);
  env_stats_.peak_gpu_mem = std::max(env_stats_.peak_gpu_mem, stats.peak_memory_usage);
  env_stats_.icp_time += (double) stats.icp_runtime;
}
void EnvObjectRecognition::PrintStateGPU(GraphState state)
{
  std::vector<std::vector<uint8_t>> random_color(3);
  std::vector<uint8_t> random_red(kCameraWidth * kCameraHeight, 255);
  std::vector<uint8_t> random_green(kCameraWidth * kCameraHeight, 255);
  std::vector<uint8_t> random_blue(kCameraWidth * kCameraHeight, 255);
  random_color[0] = random_red;
  random_color[1] = random_green;
  random_color[2] = random_blue;
  std::vector<int32_t> random_depth(kCameraWidth * kCameraHeight, 0);
  std::vector<int> random_poses_occluded(1, 0);
  std::vector<int> random_poses_occluded_other(1, 0);
  std::vector<float> random_poses_clutter_cost(1, 0);

  std::vector<ObjectState> objects = state.object_states();

  //// Need to init source color and depth for root level, where no object is present in source state
  // std::vector<std::vector<uint8_t>> source_result_color(3);
  // source_result_color[0] = random_red;
  // source_result_color[1] = random_green;
  // source_result_color[2] = random_blue;
  // By default source is set to max depth so that nothing happens when we merge with rendered
  std::vector<std::vector<uint8_t>> source_result_color;
  // std::vector<int32_t> source_result_depth(kCameraWidth * kCameraHeight, 0);
  std::vector<int32_t> source_result_depth;

  // GetStateImagesGPU(
  //   objects, random_color, random_depth, 
  //   source_result_color, source_result_depth, random_poses_occluded, 1,
  //   random_poses_occluded_other, random_poses_clutter_cost
  // );
  float* random_float;
  uint8_t* random_uint8;
  int* random_int;
  int random_int_num;
  vector<cuda_renderer::Model::mat4x4> random_adjusted_poses;
  GetStateImagesUnifiedGPU(
    "RENDER",
    objects,
    random_color,
    random_depth,
    source_result_color,
    source_result_depth,
    1,
    random_poses_clutter_cost,
    random_float,
    random_uint8,
    random_int_num,
    random_int,
    random_int,
    // rendered_preicp_cost_gpu,
    // observed_preicp_cost_gpu,
    // points_diff_preicp_cost_gpu,
    random_adjusted_poses,
    random_float,
    random_float,
    random_float,
    perch_params_.sensor_resolution,
    False, // Do ICP inside gpu for 6dof
    0,
    False
  );
  PrintGPUImages(source_result_depth, source_result_color, 1, "graph_state", random_poses_occluded);
}
void EnvObjectRecognition::ComputeGreedyCostsInParallelGPU(const std::vector<int32_t> &source_result_depth,
                                                          const std::vector<ObjectState> &last_object_states,
                                                          std::vector<CostComputationOutput> &output,
                                                          int batch_index) {
    /*
     * Calculate cost parallely on GPU using PERCH 2.0 flow
     */
    std::cout << "Computing costs in parallel GPU" << endl;

    
    std::vector<uint8_t> random_red(kCameraWidth * kCameraHeight, 255);
    std::vector<uint8_t> random_green(kCameraWidth * kCameraHeight, 255);
    std::vector<uint8_t> random_blue(kCameraWidth * kCameraHeight, 255);

    //// Initialize source image (input image) color randomly because its not used anywhere
    std::vector<std::vector<uint8_t>> source_result_color(3);
    source_result_color[0] = random_red;
    source_result_color[1] = random_green;
    source_result_color[2] = random_blue;
    
    int source_id = 0;

    // Storing ICP adjusted poses on CPU
    vector<ObjectState> modified_last_object_states;
    // Storing ICP adjusted poses done on GPU
    std::vector<cuda_renderer::Model::mat4x4> gpu_icp_adjusted_poses;

    int num_poses = last_object_states.size();
    // For storing cost after ICP
    std::vector<float> rendered_cost_gpu_vec(num_poses, -1);
    float* rendered_cost_gpu = rendered_cost_gpu_vec.data();
    std::vector<float> observed_cost_gpu_vec(num_poses, 0);
    float* observed_cost_gpu = observed_cost_gpu_vec.data();

    // For storing cost before doing ICP - not being used
    std::vector<float> rendered_preicp_cost_gpu_vec(num_poses, -1);
    float* rendered_preicp_cost_gpu = rendered_preicp_cost_gpu_vec.data();
    std::vector<float> observed_preicp_cost_gpu_vec(num_poses, 0);    
    float* observed_preicp_cost_gpu = observed_preicp_cost_gpu_vec.data();

    // Not being used
    std::vector<int> total_cost(num_poses, 0);
    float* points_diff_cost_gpu;
    float* points_diff_preicp_cost_gpu;
    std::vector<int> poses_occluded(num_poses, 0);
    std::vector<int> poses_occluded_other(num_poses, 1);
    std::vector<int> adjusted_poses_occluded(num_poses, 0);
    std::vector<int> adjusted_poses_occluded_other(num_poses, 0);
    std::vector<float> pose_clutter_cost(num_poses, 0.0);

    // For storing debug stuff, images before and after ICP
    std::vector<std::vector<uint8_t>> result_color, adjusted_result_color;
    std::vector<int32_t> result_depth, adjusted_result_depth;

    // For storing debug stuff, point clouds before ICP
    float* result_cloud;
    uint8_t* result_cloud_color;
    int* dc_index;
    int* cloud_pose_map; // Mapping every point in rendered cloud to a pose index
    int rendered_point_num;

    // Stage is cloud if ICP is on CPU
    string stage = "CLOUD";

    // If ICP on GPU then directly get cost after first rendering flow
    if (perch_params_.icp_type == 3) {
      stage = "COST";
    }
    if (perch_params_.vis_expanded_states) {
      // In this case, all debug data will be copied to CPU
      stage = "DEBUG";
    } 

    // Convert different types of costs to a single variable that is used as a switch
    int cost_type;
    bool calc_obs_cost = false;
    if (env_params_.use_external_pose_list == 1)
    {
      // 6-Dof - depth only based cost and also use observed cost
      cost_type = 2;
      calc_obs_cost = true;
    }
    else
    {
      // 3-DOF
      if (perch_params_.use_color_cost)
      {
        // Sameshape scenes
        cost_type = 1;
      }
      else
      {
        cost_type = 0;
      }
      calc_obs_cost = true;
    }
    printf("Using cost type : %d\n", cost_type);

    GetStateImagesUnifiedGPU(
      stage,
      last_object_states,
      source_result_color,
      source_result_depth,
      result_color,
      result_depth,
      0,
      pose_clutter_cost,
      result_cloud,
      result_cloud_color,
      rendered_point_num,
      dc_index,
      cloud_pose_map,
      // rendered_preicp_cost_gpu,
      // observed_preicp_cost_gpu,
      // points_diff_preicp_cost_gpu,
      gpu_icp_adjusted_poses,
      rendered_cost_gpu,
      observed_cost_gpu,
      points_diff_cost_gpu,
      perch_params_.sensor_resolution,
      (perch_params_.icp_type == 3), // Do ICP inside gpu for 6dof
      cost_type,
      calc_obs_cost
    );

    if (perch_params_.vis_expanded_states) {
      // For debug print states after the flow 
      // For GPU ICP, this will be final poses
      // For CPU ICP these will be poses before ICP
      PrintGPUImages(
        result_depth, result_color, num_poses, 
        "succ_" + std::to_string(source_id) + "_batch_" + std::to_string(batch_index), adjusted_poses_occluded,
        total_cost);
    }

    // PrintGPUClouds(
    //   last_object_states, result_cloud, result_cloud_color, depth_data, dc_index, 
    //   num_poses, rendered_point_num, gpu_stride, poses_occluded_other.data(), "rendered",
    //   modified_last_object_states, true, render_point_cloud_topic, false
    // );

    if (perch_params_.icp_type != 3) {
      // ICP on CPU, need to do ICP, then run flow again to calculate cost
      GetICPAdjustedPosesCPU(
        last_object_states, num_poses, result_cloud, result_cloud_color, 
        rendered_point_num, cloud_pose_map, poses_occluded_other.data(),
        modified_last_object_states, true, render_point_cloud_topic, false
      );
    
      // vector<ObjectState> random_modified_last_object_states;
      // PrintGPUClouds(
      //   modified_last_object_states, result_cloud, result_cloud_color, depth_data, dc_index, 
      //   num_poses, rendered_point_num, gpu_stride, poses_occluded_ptr, "rendered",
      //   random_modified_last_object_states, false, render_point_cloud_topic, true
      // );

      stage = "COST";
      if (perch_params_.vis_expanded_states) {
        stage = "DEBUG";
      }    
      GetStateImagesUnifiedGPU(
        stage,
        modified_last_object_states,
        source_result_color,
        source_result_depth,
        adjusted_result_color,
        adjusted_result_depth,
        0,
        pose_clutter_cost,
        result_cloud,
        result_cloud_color,
        rendered_point_num,
        dc_index,
        cloud_pose_map,
        gpu_icp_adjusted_poses,
        rendered_cost_gpu,
        observed_cost_gpu,
        points_diff_cost_gpu,
        perch_params_.sensor_resolution,
        false,
        cost_type,
        calc_obs_cost
      );

      if (perch_params_.vis_expanded_states) {
        // Print images after ICP
        PrintGPUImages(
          adjusted_result_depth, adjusted_result_color, num_poses, 
          "succ_icp_" + std::to_string(source_id) + "_batch_" + std::to_string(batch_index), adjusted_poses_occluded,
          total_cost);
      }
    }


    vector<unsigned short> source_depth_image;
    source_depth_image.push_back(1);
    
    Eigen::Isometry3d cam_z_front, cam_to_body;
    cam_to_body.matrix() << 0, 0, 1, 0,
                    -1, 0, 0, 0,
                    0, -1, 0, 0,
                    0, 0, 0, 1;
    cam_z_front = cam_to_world_ * cam_to_body;

    // Copy cost and other outputs to CostComputationOutput objects
    for(int i = 0; i < num_poses; i++){
      CostComputationOutput cur_unit;
      ObjectState object_state;
      if (perch_params_.icp_type != 3) {
        // If ICP was done on CPU, we already have the final pose
        object_state = modified_last_object_states[i];
      } else {
        // Geting ICP adjusted pose directly from GPU
        // Need to remove preprocessing transform because its treated as a separate transform in this file
        Eigen::Matrix4f transformation_new = gpu_icp_adjusted_poses[i].to_eigen(100);

        if (env_params_.use_external_pose_list != 1)
        {
          // Get the ICP transform into the original frame (table) for 3dof so that it can be applied
          int model_id = last_object_states[i].id();
          Eigen::Matrix4d preprocess_transform = 
            obj_models_[model_id].preprocessing_transform().matrix().cast<double>();
          transformation_new = cam_z_front.matrix().cast<float>() * transformation_new;
          transformation_new = transformation_new * preprocess_transform.inverse().cast<float>();
        }

        Quaternionf quaternion(transformation_new.block<3,3>(0,0));
        ContPose pose_out = ContPose(
          transformation_new(0, 3), transformation_new(1, 3), transformation_new(2, 3), 
          quaternion.x(), quaternion.y(), quaternion.z(), quaternion.w()
        );

        ObjectState modified_object_state(
          last_object_states[i].id(),
          last_object_states[i].symmetric(), pose_out, 
          last_object_states[i].segmentation_label_id()
        );
        object_state = modified_object_state;
      }
      // TODO : fix less than 0 case, happens in greedy when no points in rendered scene for object
      if ((int) rendered_cost_gpu[i] < 0)
      {
        cout << "Invalid " << object_state << endl;
        printf("Pose %d was invalid\n", i);
        cur_unit.cost = -1;
      }
      else
      {
        if ((int)rendered_cost_gpu[i] == 100 && (int)observed_cost_gpu[i] == 100)
        {
          points_diff_cost_gpu[i] = 100.0;
        }
        // cur_unit.cost = (int) (rendered_cost_gpu[i] + observed_cost_gpu[i] + abs(points_diff_cost_gpu[i]));
        cur_unit.cost = (int) (rendered_cost_gpu[i] + observed_cost_gpu[i]);
        cur_unit.preicp_cost = (int) (rendered_preicp_cost_gpu[i] + observed_preicp_cost_gpu[i]);
      }
      GraphState greedy_state;
      greedy_state.AppendObject(object_state);
      cur_unit.adjusted_state = greedy_state;
      cur_unit.state_properties.last_max_depth = 0;
      cur_unit.state_properties.last_min_depth = 0;
      cur_unit.state_properties.target_cost =  (int) rendered_cost_gpu[i];
      cur_unit.state_properties.source_cost = (int) observed_cost_gpu[i];

      cur_unit.state_properties.preicp_target_cost =  (int) rendered_preicp_cost_gpu[i];
      cur_unit.state_properties.preicp_source_cost = (int) observed_preicp_cost_gpu[i];
      cur_unit.state_properties.last_level_cost = (int) points_diff_cost_gpu[i];
      cur_unit.depth_image = source_depth_image;
      output[i + batch_index] = cur_unit;
    }
}

void EnvObjectRecognition::ComputeCostsInParallelGPU(std::vector<CostComputationInput> &input,
                                                  std::vector<CostComputationOutput> *output,
                                                  bool lazy) {
  /* Deprecated
   * Compute Costs using GPU for different levels of scene tree
   */
  std::cout << "Computing costs in parallel GPU" << endl;

  // std::vector<cuda_renderer::Model::mat4x4> mat4_v, source_mat4_v;
  // std::vector<ContPose> contposes;
  // std::cout << input_depth_image_path << endl;
  // cv::Mat cv_depth_image = cv::imread(input_depth_image_path, CV_32SC1);
  // cv::Mat cv_depth_image = cv::imread(input_depth_image_path, CV_LOAD_IMAGE_UNCHANGED);
  // int num_poses = 5;
  std::vector<ObjectState> source_last_object_states;
  std::vector<ObjectState> objects = input[0].source_state.object_states();
  
  // Random is used as source for merging when rendering the source itself
  std::vector<std::vector<uint8_t>> random_color(3);
  std::vector<uint8_t> random_red(kCameraWidth * kCameraHeight, 255);
  std::vector<uint8_t> random_green(kCameraWidth * kCameraHeight, 255);
  std::vector<uint8_t> random_blue(kCameraWidth * kCameraHeight, 255);
  random_color[0] = random_red;
  random_color[1] = random_green;
  random_color[2] = random_blue;
  std::vector<int32_t> random_depth(kCameraWidth * kCameraHeight, 0);
  std::vector<int> random_poses_occluded(1, 0);
  std::vector<int> random_poses_occluded_other(1, 0);
  std::vector<float> random_poses_clutter_cost(1, 0);
  int* cloud_label;


  //// Need to init source color and depth for root level, where no object is present in source state
  std::vector<std::vector<uint8_t>> source_result_color(3);
  source_result_color[0] = random_red;
  source_result_color[1] = random_green;
  source_result_color[2] = random_blue;
  // By default source is set to max depth so that nothing happens when we merge with rendered
  std::vector<int32_t> source_result_depth(kCameraWidth * kCameraHeight, 0);
  
  int source_id = input[0].source_id;
  bool root_level = false;
  bool last_level = false;
  if (objects.size() == 0)
  {
    cout << "Root Source State\n";
    root_level = true;
  }
  else
  {
    cout << "Expanding " << source_id << endl;
    cout << input[0].source_state << endl;
    cout << "Num objects : " << objects.size() << endl;
    // source_last_object_states.push_back(objects[objects.size() - 1]);
    for (int i = 0; i < objects.size(); i++)
    {
      source_last_object_states.push_back(objects[i]);      
    }
    //// ADD GPU call here for GPU + Tree
    // GetStateImagesGPU(
    //   source_last_object_states, random_color, random_depth, 
    //   source_result_color, source_result_depth, random_poses_occluded, 1,
    //   random_poses_occluded_other, random_poses_clutter_cost
    // );
    if (perch_params_.vis_expanded_states) {
      PrintGPUImages(source_result_depth, source_result_color, 1, "expansion_" + std::to_string(source_id), random_poses_occluded);
    }
    // return;
  }
  if (objects.size() == env_params_.num_objects)
  {
    last_level = true;
  }



  std::vector<ObjectState> last_object_states;
  vector<ObjectState> modified_last_object_states;

  int num_poses = input.size();
  std::vector<int> rendered_cost(num_poses, 0);
  std::vector<float> rendered_cost_gpu_vec(num_poses, -1);
  float* rendered_cost_gpu = rendered_cost_gpu_vec.data();
  uint8_t* observed_explained;
  std::vector<int> last_level_cost(num_poses, 0);
  std::vector<float> observed_cost_gpu_vec(num_poses, 0);
  float* observed_cost_gpu = observed_cost_gpu_vec.data();
  std::vector<int> total_cost(num_poses, 0);

  std::vector<int> poses_occluded(num_poses, 0);
  std::vector<int> poses_occluded_other(num_poses, 0);
  std::vector<int> adjusted_poses_occluded(num_poses, 0);
  std::vector<int> adjusted_poses_occluded_other(num_poses, 0);
  std::vector<int> pose_segmentation_label(num_poses, 0);
  std::vector<float> pose_observed_points_total(num_poses, 0.0);
  std::vector<float> pose_clutter_cost(num_poses, 0.0);

  for(int i = 0; i < num_poses; i ++) {
    // For non-root level, this should be adjusted state
    std::vector<ObjectState> objects = input[i].child_state.object_states();
    const ObjectState &last_object_state = objects[objects.size() - 1];
    int model_id = last_object_state.id();
    if (env_params_.use_external_pose_list == 1)
    {
      // Store total observed points per pose using segmentation

      string model_name = obj_models_[model_id].name();
      // TODO : store this mapping in a map
      int required_object_id = distance(segmented_object_names.begin(), 
      find(segmented_object_names.begin(), segmented_object_names.end(), model_name)); 
      pose_segmentation_label[i] = required_object_id + 1;
      pose_observed_points_total[i] = segmented_observed_point_count[required_object_id];
    }

    if (!root_level)
    {
      // Find the icp adjusted state that would have been computed at the root level
      GraphState single_object_graph_state;
      single_object_graph_state.AppendObject(last_object_state);
      assert(adjusted_single_object_state_cache_.find(single_object_graph_state) !=
           adjusted_single_object_state_cache_.end());
      GraphState adjusted_last_object_state =
        adjusted_single_object_state_cache_[single_object_graph_state];
      const auto &last_object = adjusted_last_object_state.object_states().back();
      last_object_states.push_back(last_object);

    }
    else
    {
      last_object_states.push_back(last_object_state);
    }
    
  }

  // std::cout << "exclusive sum: ";
  // std::exclusive_scan(tris_model_count.begin(), tris_model_count.end(), std::ostream_iterator<int>(std::cout, " "), 0);
  // #ifdef CUDA_ON
  // int render_size = 750;
  // int total_render_num = mat4_v.size();
  // int num_render = (total_render_num-1)/render_size+1;
  // std::vector<int> total_result_cost;
  // for(int i =0; i < num_render; i ++){
    // auto last = std::min(total_render_num, i*render_size + render_size);
    // std::vector<cuda_renderer::Model::mat4x4>::const_iterator start = mat4_v.begin() + i*render_size;
    // std::vector<cuda_renderer::Model::mat4x4>::const_iterator finish = mat4_v.begin() + last;
    // std::vector<cuda_renderer::Model::mat4x4> cur_transform(start,finish);
    std::vector<std::vector<uint8_t>> result_color, adjusted_result_color;
    std::vector<int32_t> result_depth, adjusted_result_depth;

    if (env_params_.use_external_pose_list == 0)
    {
      pose_segmentation_label.clear();
    }

    //// ADD GPU call here for GPU + Tree
    // GetStateImagesGPU(
    //   last_object_states, source_result_color, source_result_depth, 
    //   result_color, result_depth, poses_occluded, 0,
    //   poses_occluded_other, pose_clutter_cost, pose_segmentation_label
    // );
    // if (objects.size() == 3)
    // if (perch_params_.vis_expanded_states) {
    //   PrintGPUImages(result_depth, result_color, num_poses, "succ_" + std::to_string(source_id), poses_occluded);
    // }

    // // Compute observed cloud
    // free(dc_index);
    // float* result_observed_cloud = (float*) malloc(gpu_point_dim * env_params_.width*env_params_.height * sizeof(float));
    // int* observed_dc_index;
    // int32_t* observed_depth_data = cv_input_filtered_depth_image.ptr<int>(0);
    // int observed_point_num;
    // cuda_renderer::depth2cloud_global(
    //   observed_depth_data, result_observed_cloud, observed_dc_index, observed_point_num, env_params_.width, env_params_.height, 
    //   1, random_poses_occluded.data(), kCameraCX, kCameraCY, kCameraFX, kCameraFY, gpu_depth_factor, gpu_stride, gpu_point_dim
    // );
    // if (root_level)
    // PrintGPUClouds(result_observed_cloud, observed_depth_data, observed_dc_index, 1, observed_point_num, gpu_stride, random_poses_occluded.data(), "observed");

    int num_valid_poses = poses_occluded.size() - accumulate(poses_occluded.begin(), poses_occluded.end(), 0);
    printf("Num valid poses : %d\n", num_valid_poses);
    // if (accumulate(poses_occluded.begin(), poses_occluded().end(), 0) > 0)
    // {
      // Compute clouds needed for Pre-icp
    
    float* result_cloud;
    Eigen::Vector3f* result_cloud_eigen;
    uint8_t* result_cloud_color;
    int* dc_index;
    int* cloud_pose_map;
    int32_t* depth_data = result_depth.data();
    int rendered_point_num;
    int k = 1;
    int* poses_occluded_ptr = poses_occluded.data();

      //// Convert to point cloud
      cuda_renderer::depth2cloud_global(
        result_depth, result_color, result_cloud_eigen, result_cloud, result_cloud_color, dc_index, rendered_point_num, cloud_pose_map, cloud_label, env_params_.width, env_params_.height, 
        num_poses, poses_occluded, kCameraCX, kCameraCY, kCameraFX, kCameraFY, gpu_depth_factor, gpu_stride, gpu_point_dim
      );

      // PrintGPUClouds(
      //   last_object_states, result_cloud, result_cloud_color, depth_data, dc_index, 
      //   num_poses, rendered_point_num, gpu_stride, poses_occluded_ptr, "rendered",
      //   modified_last_object_states, false, render_point_cloud_topic
      // );
      // Do ICP if at root
      // if (root_level && true)
      if (root_level)
      {
        //// If root then we need to do ICP for all, fill with dummy ones
        fill(poses_occluded_other.begin(), poses_occluded_other.end(), 1);
      }
      {
        //// Do ICP for occluded stuff, poses which are not occluded by other would be same as original
        // PrintGPUClouds(
        //   last_object_states, result_cloud, result_cloud_color, depth_data, dc_index, 
        //   num_poses, rendered_point_num, gpu_stride, poses_occluded_other.data(), "rendered",
        //   modified_last_object_states, true, render_point_cloud_topic, false
        // );
        
        GetICPAdjustedPosesCPU(
          last_object_states, num_poses, result_cloud, result_cloud_color, 
          rendered_point_num, cloud_pose_map, poses_occluded_other.data(),
          modified_last_object_states, true, render_point_cloud_topic, false
        );
        //// ADD GPU call here for GPU + Tree
        // GetStateImagesGPU(
        //   modified_last_object_states, source_result_color, source_result_depth, 
        //   adjusted_result_color, adjusted_result_depth, adjusted_poses_occluded, 0,
        //   adjusted_poses_occluded_other, pose_clutter_cost, pose_segmentation_label
        // );
        // if (source_id == 136)
        // if (objects.size() == 2)
        // PrintGPUImages(adjusted_result_depth, adjusted_result_color, num_poses, "icp_succ_" + std::to_string(source_id), adjusted_poses_occluded);

        // free(result_cloud);
        // free(dc_index);
        // free(depth_data);
        depth_data = adjusted_result_depth.data();
        poses_occluded_ptr = adjusted_poses_occluded.data();
        // Override adjusted clouds to same variable
        cuda_renderer::depth2cloud_global(
          adjusted_result_depth, adjusted_result_color, result_cloud_eigen, result_cloud, result_cloud_color, dc_index, rendered_point_num, cloud_pose_map, cloud_label,
          env_params_.width, env_params_.height, 
          num_poses, adjusted_poses_occluded, kCameraCX, kCameraCY, kCameraFX, kCameraFY, gpu_depth_factor, gpu_stride, gpu_point_dim
        );
        
        num_valid_poses = adjusted_poses_occluded.size() - accumulate(adjusted_poses_occluded.begin(), adjusted_poses_occluded.end(), 0);
        printf("Num valid poses after icp : %d\n", num_valid_poses);
        
        
        // vector<ObjectState> random_modified_last_object_states;
        // PrintGPUClouds(
        //   modified_last_object_states, result_cloud, result_cloud_color, depth_data, dc_index, 
        //   num_poses, rendered_point_num, gpu_stride, poses_occluded_ptr, "rendered",
        //   random_modified_last_object_states, false, render_point_cloud_topic, true
        // );

        // GetICPAdjustedPosesGPU(result_cloud,
        //                       dc_index,
        //                       depth_data,
        //                       num_poses,
        //                       result_observed_cloud,
        //                       observed_dc_index,
        //                       rendered_point_num,
        //                       poses_occluded.data());
      }
    if (num_valid_poses > 0)
    {
      // Do KNN
      float* knn_dist   = (float*) malloc(rendered_point_num * k * sizeof(float));
      int* knn_index  = (int*)   malloc(rendered_point_num * k * sizeof(int));
      // int observed_nb = env_params_.width * env_params_.height;
      // int rendered_nb = num_poses * env_params_.width * env_params_.height;
      // cuda_renderer::knn_cuda_global(
      //   result_observed_cloud, observed_point_num, result_cloud, 
      //   rendered_point_num, gpu_point_dim, k, knn_dist, knn_index
      // );
      // for(int i = 0; i < rendered_point_num; i++){
      //   printf("knn dist:%f\n", knn_dist[i]);
      // }
      // }
      // cuda_renderer::knn_test(result_observed_cloud, observed_point_num, result_cloud, rendered_point_num, gpu_point_dim, k, knn_dist, knn_index);
      cout << "KNN done\n";
      float sensor_resolution = perch_params_.sensor_resolution;
      //// Count parallely how many points are explained in color and distance
      int cost_type;
      bool calc_obs_cost = false;
      if (env_params_.use_external_pose_list == 1)
      {
        cost_type = 2;
        calc_obs_cost = true;
      }
      else
      {
        if (perch_params_.use_color_cost)
        {
          cost_type = 1;
        }
        else
        {
          cost_type = 0;
        }
        calc_obs_cost = false;
      }
      printf("Using cost type : %d\n", cost_type);
      // Put something related to cost here
      // cuda_renderer::compute_rgbd_cost(
      //   sensor_resolution, knn_dist, knn_index, 
      //   poses_occluded_ptr, cloud_pose_map,
      //   result_observed_cloud, result_observed_cloud_color,
      //   result_cloud, result_cloud_color,
      //   rendered_point_num, observed_point_num,
      //   num_poses, rendered_cost_gpu, pose_observed_points_total,
      //   observed_cost_gpu, pose_segmentation_label.data(),
      //   result_observed_cloud_label, cost_type, calc_obs_cost
      // );

      // for(int i = 0; i < num_poses; i++){
      //   total_cost[i] =  (int) (rendered_cost_gpu[i] + observed_cost_gpu[i] + pose_clutter_cost[i]);
      //   // printf("total cost :%d\n", total_cost[i]);
      // }

      if (perch_params_.vis_expanded_states) {
        PrintGPUImages(
          adjusted_result_depth, adjusted_result_color, num_poses, 
          "succ_" + std::to_string(source_id), adjusted_poses_occluded,
          total_cost);
      }
      
    }

  assert(output != nullptr);
  output->clear();
  output->resize(num_poses);
  std::vector<CostComputationOutput> output_gpu;
  vector<unsigned short> source_depth_image;
  source_depth_image.push_back(1);
  source_depth_image.push_back(1);
  source_depth_image.push_back(1);
  source_depth_image.push_back(1);
  source_depth_image.push_back(1);
  source_depth_image.push_back(1);
  source_depth_image.push_back(1);
  // vector<vector<unsigned char>> source_color_image;
  // cv::Mat source_cv_depth_image;
  // cv::Mat source_cv_color_image;
  // int total_pixel = env_params_.height*env_params_.width;

  for(int i = 0; i < num_poses; i++){
    CostComputationOutput cur_unit;
    // cur_unit.cost = total_result_cost[i];
    // TODO : fix less than 0 case, happens in greedy when no points in rendered scene for object
    if (poses_occluded_ptr[i] || (int) rendered_cost_gpu[i] < 0)
    {
      printf("Pose %d was invalid\n", i);
      cur_unit.cost = -1;
    }
    else
    {
      if (last_level)
      {
        cur_unit.cost = (int) (rendered_cost_gpu[i] + last_level_cost[i] + observed_cost_gpu[i]);
        // cur_unit.cost = rendered_cost[i] + last_level_cost[i] + observed_cost[i];
      }
      else
      {
        // cur_unit.cost = rendered_cost[i] + observed_cost[i];
        cur_unit.cost = (int) (rendered_cost_gpu[i] + observed_cost_gpu[i]);
        // cur_unit.cost = (int) (rendered_cost_gpu[i] + observed_cost_gpu[i] + pose_clutter_cost[i]);
      }
    }
    // if (root_level)
    {
      cur_unit.adjusted_state = input[i].child_state;
      cur_unit.adjusted_state.mutable_object_states()[input[i].child_state.object_states().size()-1]
        = modified_last_object_states[i];
    }
    // else
    // {
    //   // Need to make last object equal to restored adjusted state from cache
    //   cur_unit.adjusted_state = input[i].child_state;
    //   cur_unit.adjusted_state.mutable_object_states()[input[i].child_state.object_states().size()-1] 
    //     = last_object_states[i];
    // }
    cur_unit.state_properties.last_max_depth = 0;
    cur_unit.state_properties.last_min_depth = 0;
    // cur_unit.state_properties.target_cost =  total_result_cost[i];
    // cur_unit.state_properties.target_cost =  rendered_cost[i];
    cur_unit.state_properties.target_cost =  (int) rendered_cost_gpu[i];
    cur_unit.state_properties.source_cost = (int) observed_cost_gpu[i];
    // cur_unit.state_properties.last_level_cost = last_level_cost[i];
    cur_unit.state_properties.last_level_cost = (int) pose_clutter_cost[i];
    cur_unit.depth_image = source_depth_image;
    // std::vector<int32_t>::const_iterator start = result_depth.begin() + i*env_params_.width*env_params_.height;
    // std::vector<int32_t>::const_iterator finish = start + env_params_.width*env_params_.height;
    // std::vector<int32_t> curr_depth_image(start,finish);
    // cur_unit.gpu_depth_image = curr_depth_image;
    output_gpu.push_back(cur_unit);
    // output_gpu[i].color_image = ,
    // output_gpu[i].unadjusted_depth_image,
    // output_gpu[i].unadjusted_color_image
  }
  *output = output_gpu;
  // #endif

}

GraphState EnvObjectRecognition::ComputeGreedyRenderPoses() {
  nlohmann::json cost_dump;
  cost_dump["poses"] =  nlohmann::json::array();

  chrono::time_point<chrono::system_clock> start, end;
  start = chrono::system_clock::now();

  GraphState source_state;
  vector<GraphState> candidate_succs;
  GenerateSuccessorStates(source_state, &candidate_succs);
  env_stats_.scenes_rendered += static_cast<int>(candidate_succs.size());

  vector<int> candidate_succ_ids, candidate_costs;
  candidate_succ_ids.resize(candidate_succs.size(), 0);


  candidate_costs.resize(candidate_succ_ids.size());

  // Prepare the cost computation input vector.
  vector<ObjectState> last_object_states(candidate_succ_ids.size());
  vector<CostComputationOutput> cost_computation_output(candidate_succ_ids.size());

  // Initialize source image with observed depth image for occlusion handling
  std::vector<int32_t> source_result_depth(kCameraWidth * kCameraHeight, 0);
  // source_result_depth.assign(unfiltered_depth_data, unfiltered_depth_data + kCameraWidth * kCameraHeight-1);
  if (env_params_.use_external_pose_list == 1)
  {    
    float division_factor = input_depth_factor/gpu_depth_factor;
    printf("Using depth division factor : %f\n", division_factor);
    for (int i = 0; i < input_depth_image_vec.size(); i++)
    {
      // TODO : fix this hardcode
      // ycb has depth factor of 10000, and gpu is using 100, divide by 100 to get it to gpu's depth factor
      input_depth_image_vec[i] /= division_factor;
      // printf("source depth : %d\n", input_depth_image_vec[i]);
    }
  }
  for (size_t ii = 0; ii < last_object_states.size(); ++ii) {
    last_object_states[ii] =
       candidate_succs[ii].object_states()[candidate_succs[ii].object_states().size() - 1];
  }
  // int gpu_batch_size = 2000;
  int num_batches = candidate_succ_ids.size()/perch_params_.gpu_batch_size + 1;
  printf("Num GPU batches for given batch size : %d\n", num_batches);
  for (int bi = 0; bi < num_batches; bi++)
  {
    int start_index = bi * perch_params_.gpu_batch_size;
    vector<ObjectState>::const_iterator batch_start = last_object_states.begin() + start_index;
    // Take min of gpu batch size of number of poses left
    int end_index = std::min((bi + 1) * perch_params_.gpu_batch_size, (int) candidate_succ_ids.size());
    if (end_index > candidate_succ_ids.size() || start_index >= candidate_succ_ids.size()) break;

    vector<ObjectState>::const_iterator batch_end = last_object_states.begin() + end_index;
    vector<ObjectState> batch_last_object_states(batch_start, batch_end);
    printf("\n\nGetting costs for GPU batch : %d, num poses : %d\n", bi, batch_last_object_states.size());

    // vector<ObjectState>::iterator batch_start_cost_comp = cost_computation_output.begin() + bi * 10;
    // vector<ObjectState>::iterator batch_end_cost_comp = batch_start_cost_comp + (bi + 1) * 10;
    // vector<CostComputationOutput> batch_cost_computation_output(batch_start_cost_comp, batch_end_cost_comp);

    ComputeGreedyCostsInParallelGPU(input_depth_image_vec, batch_last_object_states, cost_computation_output, start_index);
    // ComputeGreedyCostsInParallelGPU(source_result_depth, batch_last_object_states, cost_computation_output, start_index);
    batch_last_object_states.clear();
  }


  // vector<CostComputationOutput> cost_computation_output;
  // ComputeGreedyCostsInParallelGPU(source_result_depth, last_object_states, &cost_computation_output);


  // std::sort(cost_computation_output.begin(), cost_computation_output.end(), compareCostComputationOutput);
  // ComputeCostsInParallelGPU(cost_computation_input, &cost_computation_output, false);
  ObjectState temp(-1, false, DiscPose(0, 0, 0, 0, 0, 0));
  vector<int> lowest_cost_per_object(env_params_.num_models, INT_MAX);
  vector<int> lowest_preicp_cost_per_object(env_params_.num_models, INT_MAX);
  vector<ObjectState> lowest_cost_state_per_object(env_params_.num_models, temp);

  vector<int> lowest_cost_state_id_per_object(env_params_.num_models, -1);
  vector<int> lowest_preicp_cost_state_id_per_object(env_params_.num_models, -1);
  printf("State number,     label     preicp_target_cost    preicp_source_cost     target_cost    source_cost    last_level_cost    preicp_candidate_costs    candidate_costs\n");
  for (size_t ii = 0; ii < candidate_succ_ids.size(); ++ii) {
      nlohmann::json pose_dump;
      const auto &output_unit = cost_computation_output[ii];
      const auto &adjusted_state = cost_computation_output[ii].adjusted_state;
      const auto &adjusted_object_state = adjusted_state.object_states().back();
      // const auto &input_unit = last_object_states[ii];
      int model_id = adjusted_object_state.id();
      string model_name = obj_models_[model_id].name();

      // candidate_succ_ids[ii] = hash_manager_.GetStateIDForceful(
      //                       input_unit);

      if (output_unit.cost== -1 || output_unit.cost== -2) {
        continue;  // Invalid successor
      }

      if (env_params_.use_external_pose_list == 1)
      {
        if (output_unit.cost < lowest_cost_per_object[model_id]
        && abs(output_unit.state_properties.target_cost - output_unit.state_properties.source_cost) < 30)
        {
          lowest_cost_per_object[model_id] = output_unit.cost;
          lowest_cost_state_per_object[model_id] = adjusted_object_state;
          lowest_cost_state_id_per_object[model_id] = ii;
        }
        if (output_unit.preicp_cost < lowest_preicp_cost_per_object[model_id]
        && abs(output_unit.state_properties.preicp_target_cost - output_unit.state_properties.preicp_source_cost) < 30)
        {
          lowest_preicp_cost_per_object[model_id] = output_unit.preicp_cost;
          lowest_preicp_cost_state_id_per_object[model_id] = ii;
        }
      }
      else
      {
        if (output_unit.cost < lowest_cost_per_object[model_id] 
        && abs(output_unit.state_properties.target_cost - output_unit.state_properties.source_cost) < 30)
        // && output_unit.cost > 0)
        {
          lowest_cost_per_object[model_id] = output_unit.cost;
          lowest_cost_state_per_object[model_id] = adjusted_object_state;
        }
      }

      if (image_debug_) {

        printf("State %d           %s       %d      %d       %d      %d      %d      %d      %d\n",
              ii,
              model_name.c_str(),
              output_unit.state_properties.preicp_target_cost,
              output_unit.state_properties.preicp_source_cost,
              output_unit.state_properties.target_cost,
              output_unit.state_properties.source_cost,
              output_unit.state_properties.last_level_cost,
              output_unit.preicp_cost,
              output_unit.cost);

      }
      
      ObjectState object_state = 
        output_unit.adjusted_state.object_states()[output_unit.adjusted_state.object_states().size() - 1];
      auto pose = object_state.cont_pose();
      const auto &obj_model = obj_models_[object_state.id()];
      Eigen::Matrix4f pose_transform = obj_model.GetRawModelToSceneTransform(pose).matrix();
      pose_dump["id"] = ii;
      pose_dump["target_cost"] = output_unit.state_properties.target_cost;
      pose_dump["source_cost"] = output_unit.state_properties.source_cost;
      pose_dump["total_cost"] = output_unit.cost;
      vector<float> vec(pose_transform.data(), pose_transform.data() + pose_transform.rows() * pose_transform.cols());
      vector<float> trans = {pose.x(), pose.y(), pose.z()};
      vector<float> quaternion = {pose.qx(), pose.qy(), pose.qz(), pose.qw()};
      Eigen::Vector3f lie_rotation = Sophus::SO3f(obj_model.GetRawModelToSceneTransform(pose).linear()).log();
      vector<float> lie(lie_rotation.data(), lie_rotation.data() + 3);
      
      pose_dump["transform"] = vec;
      pose_dump["translation"] = trans;
      pose_dump["quaternion"] = quaternion;
      pose_dump["lie_rotation"] = lie;
      cost_dump["poses"].push_back(pose_dump);

  }
  GraphState greedy_state;
  for (int model_id = 0; model_id < env_params_.num_models; model_id++)
  {
    if (lowest_cost_per_object[model_id] < INT_MAX)
    {
      greedy_state.AppendObject(lowest_cost_state_per_object[model_id]);
      printf("Cost of lowest pre ICP cost state for %s : %d ID : %d\n", 
        obj_models_[model_id].name().c_str(),lowest_preicp_cost_per_object[model_id],
        lowest_preicp_cost_state_id_per_object[model_id]);
      printf("Cost of lowest cost state for %s : %d ID : %d\n", 
        obj_models_[model_id].name().c_str(),lowest_cost_per_object[model_id],
        lowest_cost_state_id_per_object[model_id]);

    }
  }
  end = chrono::system_clock::now();
  chrono::duration<double> elapsed_seconds = end-start;
  cout << "ComputeGreedyRenderPoses() took " << elapsed_seconds.count() << " seconds\n";
  env_stats_.time = elapsed_seconds.count();
  // env_stats_.icp_time /= num_batches;
  string fname = debug_dir_ + "depth_greedy_state.png";
  string cname = debug_dir_ + "color_greedy_state.png";
  // PrintState(greedy_state, fname, cname);
  PrintStateGPU(greedy_state);

  string jname = debug_dir_ + "cost_dump.json";
  std::ofstream jo(jname);
  jo << std::setw(4) << cost_dump << std::endl;
  return greedy_state;
}

void EnvObjectRecognition::GetLazySuccs(int source_state_id,
                                        vector<int> *succ_ids, vector<int> *costs,
                                        vector<bool> *true_costs) {
  succ_ids->clear();
  costs->clear();
  true_costs->clear();

  if (source_state_id == env_params_.goal_state_id) {
    return;
  }

  // If root node, we cannot evaluate successors lazily (i.e., need to render
  // all first level states).
  if (source_state_id == env_params_.start_state_id) {
    std::cout << "Rendering all first level states" << endl;
    GetSuccs(source_state_id, succ_ids, costs);
    true_costs->resize(succ_ids->size(), true);
    return;
  }

  GraphState source_state;

  if (adjusted_states_.find(source_state_id) != adjusted_states_.end()) {
    source_state = adjusted_states_[source_state_id];
  } else {
    source_state = hash_manager_.GetState(source_state_id);
  }

  // Ditto for penultimate state.
  if (static_cast<int>(source_state.NumObjects()) == env_params_.num_objects -
      1) {
    GetSuccs(source_state_id, succ_ids, costs);
    true_costs->resize(succ_ids->size(), true);
    return;
  }

  // If in cache, return
  auto it = succ_cache.find(source_state_id);

  if (it != succ_cache.end()) {
    *costs = cost_cache[source_state_id];

    if (static_cast<int>(source_state.NumObjects()) == env_params_.num_objects -
        1) {
      succ_ids->resize(costs->size(), env_params_.goal_state_id);
    } else {
      *succ_ids = succ_cache[source_state_id];
    }

    printf("Lazily expanding cached state: %d with %zu objects\n",
           source_state_id,
           source_state.NumObjects());
    return;
  }

  printf("Lazily expanding state: %d with %zu objects\n",
         source_state_id,
         source_state.NumObjects());

  if (perch_params_.print_expanded_states) {
    // string fname = debug_dir_ + "expansion_" + to_string(source_state_id) + ".png";
    // PrintState(source_state_id, fname);
    string fname = debug_dir_ + "expansion_depth_" + to_string(source_state_id) + ".png";
    string cname = debug_dir_ + "expansion_color_" + to_string(source_state_id) + ".png";
    PrintState(source_state_id, fname, cname);
  }

  vector<int> candidate_succ_ids;
  vector<GraphState> candidate_succs;

  GenerateSuccessorStates(source_state, &candidate_succs);

  env_stats_.scenes_rendered += static_cast<int>(candidate_succs.size());

  // We don't need IDs for the candidate succs at all.
  candidate_succ_ids.resize(candidate_succs.size(), 0);

  vector<unsigned short> source_depth_image;
  GetDepthImage(source_state, &source_depth_image);

  // Prepare the cost computation input vector.
  vector<CostComputationInput> cost_computation_input(candidate_succ_ids.size());

  for (size_t ii = 0; ii < cost_computation_input.size(); ++ii) {
    auto &input_unit = cost_computation_input[ii];
    input_unit.source_state = source_state;
    input_unit.child_state = candidate_succs[ii];
    input_unit.source_id = source_state_id;
    input_unit.child_id = candidate_succ_ids[ii];
    input_unit.source_depth_image = source_depth_image;
    input_unit.source_counted_pixels = counted_pixels_map_[source_state_id];

    const ObjectState &last_object_state =
      candidate_succs[ii].object_states().back();
    GraphState single_object_graph_state;
    single_object_graph_state.AppendObject(last_object_state);
    // Get unadjusted depth image from cache
    // This image will not be in cache if cost was -1 in GetCost at first level
    const bool valid_state = GetSingleObjectDepthImage(single_object_graph_state,
                                                       &input_unit.unadjusted_last_object_depth_image, false);

    if (!valid_state) {
      continue;
    }
    // Get adjusted depth image from cache
    GetSingleObjectDepthImage(single_object_graph_state,
                              &input_unit.adjusted_last_object_depth_image, true);
    if (kUseHistogramLazy) {
      // Get adjusted histogram score from cache
      GetSingleObjectHistogramScore(single_object_graph_state,
                                    input_unit.adjusted_last_object_histogram_score);
    }
    // Get adjusted state corresponding to unadjusted state from cache
    assert(adjusted_single_object_state_cache_.find(single_object_graph_state) !=
           adjusted_single_object_state_cache_.end());
    input_unit.adjusted_last_object_state =
      adjusted_single_object_state_cache_[single_object_graph_state];
  }

  vector<CostComputationOutput> cost_computation_output;
  ComputeCostsInParallel(cost_computation_input, &cost_computation_output, true);

  //---- PARALLELIZE THIS LOOP-----------//
  for (size_t ii = 0; ii < candidate_succ_ids.size(); ++ii) {
    const auto &output_unit = cost_computation_output[ii];
    const auto &input_unit = cost_computation_input[ii];
    candidate_succ_ids[ii] = hash_manager_.GetStateIDForceful(
                               input_unit.child_state);

    const bool invalid_state = output_unit.cost == -1;

    if (invalid_state) {
      continue;
    }

    last_object_rendering_cost_[candidate_succ_ids[ii]] =
      output_unit.state_properties.target_cost +
      output_unit.state_properties.source_cost;

    if (IsGoalState(candidate_succs[ii])) {
      succ_ids->push_back(env_params_.goal_state_id);
    } else {
      succ_ids->push_back(candidate_succ_ids[ii]);
    }

    succ_cache[source_state_id].push_back(candidate_succ_ids[ii]);
    costs->push_back(output_unit.cost);

    if (image_debug_) {
      std::stringstream ss;
      ss.precision(20);
      ss << debug_dir_ + "succ_" << candidate_succ_ids[ii] << "_lazy.png";
      PrintImage(ss.str(), output_unit.depth_image);
      // printf("State %d,       %d\n", candidate_succ_ids[ii],
      //        output_unit.cost);
      // printf("State %d,       %d      %d      %d      %d\n", candidate_succ_ids[ii],
      //        output_unit.state_properties.target_cost,
      //        output_unit.state_properties.source_cost,
      //        output_unit.cost,
      //        g_value_map_[candidate_succ_ids[ii]]);

      // auto gravity_aligned_point_cloud = GetGravityAlignedPointCloud(
      //                                      output_unit.depth_image);
      // std::stringstream cloud_ss;
      // cloud_ss.precision(20);
      // cloud_ss << debug_dir_ + "cloud_" << candidate_succ_ids[ii] << ".pcd";
      // pcl::PCDWriter writer;
      // writer.writeBinary (cloud_ss.str()  , *gravity_aligned_point_cloud);
    }
  }

  true_costs->resize(succ_ids->size(), false);

  // cache succs and costs
  cost_cache[source_state_id] = *costs;

  if (perch_params_.debug_verbose) {
    printf("Lazy succs for %d\n", source_state_id);

    for (int ii = 0; ii < static_cast<int>(succ_ids->size()); ++ii) {
      printf("%d  ,  %d\n", (*succ_ids)[ii], (*costs)[ii]);
    }

    printf("\n");
  }

  // ROS_INFO("Expanding state: %d with %d objects and %d successors",
  //          source_state_id,
  //          source_state.object_ids.size(), costs->size());
  // string fname = debug_dir_ + "expansion_" + to_string(source_state_id) + ".png";
  // PrintState(source_state_id, fname);
}

int EnvObjectRecognition::GetTrueCost(int source_state_id,
                                      int child_state_id) {

  printf("Getting true cost for edge: %d ---> %d\n", source_state_id,
         child_state_id);

  GraphState source_state;

  if (adjusted_states_.find(source_state_id) != adjusted_states_.end()) {
    source_state = adjusted_states_[source_state_id];
  } else {
    source_state = hash_manager_.GetState(source_state_id);
  }

  // Dirty trick for multiple goal states.
  if (child_state_id == env_params_.goal_state_id) {
    child_state_id = GetBestSuccessorID(source_state_id);
  }

  GraphState child_state = hash_manager_.GetState(child_state_id);
  vector<unsigned short> source_depth_image;
  cv::Mat source_cv_depth_image, source_cv_color_image;
  vector<vector<unsigned char>> source_color_image;
  GetDepthImage(source_state, &source_depth_image, &source_color_image,
                &source_cv_depth_image, &source_cv_color_image);
  vector<int> source_counted_pixels = counted_pixels_map_[source_state_id];

  CostComputationOutput output_unit;
  output_unit.cost = GetCost(source_state, child_state,
                             source_depth_image,
                             source_color_image,
                             source_counted_pixels,
                             &output_unit.child_counted_pixels, &output_unit.adjusted_state,
                             &output_unit.state_properties, &output_unit.depth_image,
                             &output_unit.color_image,
                             &output_unit.unadjusted_depth_image,
                             &output_unit.unadjusted_color_image,
                             output_unit.histogram_score);

  bool invalid_state = output_unit.cost == -1;

  if (invalid_state) {
    return -1;
  }

  adjusted_states_[child_state_id] = output_unit.adjusted_state;

  assert(output_unit.depth_image.size() != 0);
  minz_map_[child_state_id] =
    output_unit.state_properties.last_min_depth;
  maxz_map_[child_state_id] =
    output_unit.state_properties.last_max_depth;
  counted_pixels_map_[child_state_id] = output_unit.child_counted_pixels;
  g_value_map_[child_state_id] = g_value_map_[source_state_id] +
                                 output_unit.cost;

  // Cache the depth image only for single object renderings.
  if (source_state.NumObjects() == 0) {
    depth_image_cache_[child_state_id] = output_unit.depth_image;
  }

  //--------------------------------------//
  if (image_debug_) {
    std::stringstream ss;
    ss.precision(20);
    ss << debug_dir_ + "succ_" << child_state_id << ".png";
    PrintImage(ss.str(), output_unit.depth_image);
    printf("State %d,       %d      %d      %d      %d\n", child_state_id,
           output_unit.state_properties.target_cost,
           output_unit.state_properties.source_cost,
           output_unit.cost,
           g_value_map_[child_state_id]);

    // auto gravity_aligned_point_cloud = GetGravityAlignedPointCloud(
    //                                      output_unit.depth_image);
    // std::stringstream cloud_ss;
    // cloud_ss.precision(20);
    // cloud_ss << debug_dir_ + "cloud_" << modified_state_id << ".pcd";
    // pcl::PCDWriter writer;
    // writer.writeBinary (cloud_ss.str()  , *gravity_aligned_point_cloud);
  }

  // if (child_state_id != child_state_id) {
  //   printf("Modified: %d to %d\n", child_state_id, child_state_id);
  // }
  return output_unit.cost;
}

int EnvObjectRecognition::NumHeuristics() const {
  return (2 + static_cast<int>(rcnn_heuristics_.size()));
}

int EnvObjectRecognition::GetGoalHeuristic(int state_id) {
  return 0;
}

int EnvObjectRecognition::GetGoalHeuristic(int q_id, int state_id) {
  if (state_id == env_params_.start_state_id) {
    return 0;
  }

  if (state_id == env_params_.goal_state_id) {
    return 0;
  }

  GraphState s;

  if (adjusted_states_.find(state_id) != adjusted_states_.end()) {
    s = adjusted_states_[state_id];
  } else {
    s = hash_manager_.GetState(state_id);
  }

  int num_objects_left = env_params_.num_objects - s.NumObjects();
  int depth_first_heur = num_objects_left;
  // printf("State %d: %d %d\n", state_id, icp_heur, depth_first_heur);

  assert(q_id < NumHeuristics());

  switch (q_id) {
  case 0:
    return 0;

  case 1:
    return depth_first_heur;

  default: {
    const int rcnn_heuristic = rcnn_heuristics_[q_id - 2](s);

    if (rcnn_heuristic > 1e-5) {
      return kNumPixels;
    }

    return last_object_rendering_cost_[state_id];

    // return rcnn_heuristics_[q_id - 2](s);
  }

    // case 2: {
    //   // return static_cast<int>(1000 * minz_map_[state_id]);
    //   return kNumPixels - static_cast<int>(counted_pixels_map_[state_id].size());
    // }

    // default:
    //   return 0;
  }
}

bool EnvObjectRecognition::IsValidHistogram(int object_model_id,
        cv::Mat last_cv_obj_color_image, double threshold, double &base_distance)
{
  if (obj_models_[object_model_id].name().compare("pepsi_can") == 0 ||
      obj_models_[object_model_id].name().compare("7up_can") == 0) {
    printf("Pepsi model hack\n");
    threshold = 0.9;
  }
  // Get mask corresponding to non-zero pixels in rendered image
  cv::Mat mask, observed_image_segmented;
  cv::cvtColor(last_cv_obj_color_image, mask, CV_BGR2GRAY);
  mask = mask > 0;
  // cv_input_color_image.copyTo(observed_image_segmented, mask);

  // Crop rendered and observed_color image to this mask
  cv::Mat Points;
  cv::findNonZero(mask, Points);
  cv::Rect bounding_box = cv::boundingRect(Points);
  observed_image_segmented = cv_input_color_image(bounding_box);
  cv::Mat last_cv_obj_color_image_cropped = last_cv_obj_color_image(bounding_box);

  // cv::findNonZero(last_cv_obj_color_image, mask);
  // cv::threshold( src_gray, dst, threshold_value, max_BINARY_value,threshold_type );
  // cv::imshow("valid image", mask);
  int channels[] = { 0, 1 };
  int h_bins = 50; int s_bins = 60;
  int histSize[] = { h_bins, s_bins };
  cv::MatND hist_base, hist_test1;
  cv::Mat hsv_base, hsv_test1;
  float h_ranges[] = { 0, 180 };
  float s_ranges[] = { 0, 256 };
  const float* ranges[] = { h_ranges, s_ranges };

  cv::cvtColor( observed_image_segmented, hsv_base, CV_BGR2HSV );
  cv::cvtColor( last_cv_obj_color_image_cropped, hsv_test1, CV_BGR2HSV );

  cv::calcHist( &hsv_base, 1, channels, cv::Mat(), hist_base, 2, histSize, ranges, true, false );
  // Make all values between 0 and 1
  cv::normalize( hist_base, hist_base, 0, 1, cv::NORM_MINMAX, -1, cv::Mat() );

  cv::calcHist( &hsv_test1, 1, channels, cv::Mat(), hist_test1, 2, histSize, ranges, true, false );
  cv::normalize( hist_test1, hist_test1, 0, 1, cv::NORM_MINMAX, -1, cv::Mat() );

  // using bhattacharya distance, lesser value means histograms are more similar
  base_distance = cv::compareHist( hist_base, hist_test1, 3 );
  // double base_distance = cv::compareHist( hist_base, hist_test1, 2 );
  printf("Histogram comparison : %f\n", base_distance);



  if (base_distance > threshold) {
    // int random = rand() % 500 + 1;
    if (image_debug_) {
      cv::Mat merge;
      cv::hconcat(last_cv_obj_color_image_cropped, observed_image_segmented, merge);
      std::string cfname = debug_dir_ + "rejected_histogram_match_" + to_string(rejected_histogram_count) + ".png";
      cv::imwrite(cfname.c_str(), merge);
      rejected_histogram_count++;
    }
    // cv::imshow("rendered_image", merge);
    // cv::waitKey(500);
    return false;
  }
  return true;
}

int EnvObjectRecognition::GetLazyCost(const GraphState &source_state,
                                      const GraphState &child_state,
                                      const std::vector<unsigned short> &source_depth_image,
                                      const vector<vector<unsigned char>> &source_color_image,
                                      const std::vector<unsigned short> &unadjusted_last_object_depth_image,
                                      const std::vector<unsigned short> &adjusted_last_object_depth_image,
                                      const GraphState &adjusted_last_object_state,
                                      const std::vector<int> &parent_counted_pixels,
                                      const double adjusted_last_object_histogram_score,
                                      GraphState *adjusted_child_state,
                                      GraphStateProperties *child_properties,
                                      vector<unsigned short> *final_depth_image) {
  assert(child_state.NumObjects() > 0);
  final_depth_image->clear();
  *adjusted_child_state = child_state;

  child_properties->last_max_depth = kKinectMaxDepth;
  child_properties->last_min_depth = 0;

  const auto &last_object = child_state.object_states().back();
  ContPose child_pose = last_object.cont_pose();
  int last_object_id = last_object.id();

  // RGB Aditya
  cv::Mat last_cv_obj_depth_image, last_cv_obj_color_image;
  vector<unsigned short> last_obj_depth_image;
  vector<vector<unsigned char>> last_obj_color_image;
  const float *succ_depth_buffer;
  if (perch_params_.use_color_cost)
  {
    GraphState s_new_obj;
    s_new_obj.AppendObject(ObjectState(last_object_id,
                                      obj_models_[last_object_id].symmetric(), child_pose));
    succ_depth_buffer = GetDepthImage(s_new_obj, &last_obj_depth_image, &last_obj_color_image,
                                      &last_cv_obj_depth_image, &last_cv_obj_color_image);
  }

  ContPose pose_in(child_pose),
           pose_out(child_pose);
  PointCloudPtr cloud_in(new PointCloud);
  PointCloudPtr succ_cloud(new PointCloud);
  PointCloudPtr cloud_out(new PointCloud);

  unsigned short succ_min_depth, succ_max_depth;
  vector<int> new_pixel_indices;

  // RGB Aditya
  vector<unsigned short> child_depth_image;
  vector<vector<unsigned char>> child_color_image;
  if (perch_params_.use_color_cost)
  {
      GetComposedDepthImage(source_depth_image, source_color_image,
                            last_obj_depth_image, last_obj_color_image,
                            &child_depth_image, &child_color_image);
  }
  else
  {
      GetComposedDepthImage(source_depth_image, unadjusted_last_object_depth_image,
                            &child_depth_image);
      last_obj_depth_image = unadjusted_last_object_depth_image;
  }
  // GetComposedDepthImage(source_depth_image, source_color_image,
  //                     unadjusted_last_object_depth_image, last_obj_color_image,
  //                     &child_depth_image, &child_color_image);

  if (image_debug_)
  {
    std::string imn = "last_obj_depth_image.png";
    // PrintImage(imn, last_obj_depth_image, true);
  }

  if (IsOccluded(source_depth_image, child_depth_image, &new_pixel_indices,
                 &succ_min_depth,
                 &succ_max_depth)) {
    return -1;
  }

  vector<unsigned short> new_obj_depth_image(kCameraWidth *
                                             kCameraHeight, kKinectMaxDepth);

  // RGB Aditya
  vector<unsigned char> color_vector{'0','0','0'};
  vector<vector<unsigned char>> new_obj_color_image(kCameraWidth *
                                             kCameraHeight, color_vector);
  // Do ICP alignment on object *only* if it has been occluded by an existing
  // object in the scene. Otherwise, we could simply use the cached depth image corresponding to the unoccluded ICP adjustement.

  // if (static_cast<int>(new_pixel_indices.size()) != GetNumValidPixels(
  //       unadjusted_last_object_depth_image)) {
  if (static_cast<int>(new_pixel_indices.size()) != GetNumValidPixels(
        last_obj_depth_image)) {

    for (size_t ii = 0; ii < new_pixel_indices.size(); ++ii) {
      new_obj_depth_image[new_pixel_indices[ii]] =
        child_depth_image[new_pixel_indices[ii]];

      // RGB Aditya
      if (perch_params_.use_color_cost)
        new_obj_color_image[new_pixel_indices[ii]] =
          child_color_image[new_pixel_indices[ii]];
    }

    // Create point cloud (cloud_in) corresponding to new pixels.
    cloud_in = GetGravityAlignedPointCloud(new_obj_depth_image, new_obj_color_image);

    // Begin ICP Adjustment
    // ICP needs to happend with both objects in the image, so full state is needed
    GetICPAdjustedPose(cloud_in, pose_in, cloud_out, &pose_out,
                       parent_counted_pixels);

    if (cloud_in->size() != 0 && cloud_out->size() == 0) {
      printf("Error in ICP adjustment\n");
    }

    const ObjectState modified_last_object(last_object.id(),
                                           last_object.symmetric(), pose_out);
    int last_idx = child_state.NumObjects() - 1;
    adjusted_child_state->mutable_object_states()[last_idx] = modified_last_object;


    // End ICP Adjustment
    // Check again after ICP.
    if (!IsValidPose(source_state, last_object_id,
                     modified_last_object.cont_pose(), true)) {
      // cloud_out = cloud_in;
      // *adjusted_child_state = child_state;
      return -1;
    }

    // RGB Aditya
    if (perch_params_.use_color_cost) {
      int num_occluders = 0;
      succ_depth_buffer = GetDepthImage(
        *adjusted_child_state, &new_obj_depth_image, &new_obj_color_image,
        last_cv_obj_depth_image, last_cv_obj_color_image, &num_occluders, false
      );
    } else {
      // The first operation removes self occluding points, and the second one
      // removes points occluded by other objects in the scene.
      // Need to Remove other objects, so that cost computation can be lazy
      new_obj_depth_image = GetDepthImageFromPointCloud(cloud_out);
    }
    if (image_debug_)
    {
      std::string imn = "new_obj_depth_image_icp.png";
      // PrintImage(imn, new_obj_depth_image, true);
    }

    vector<int> new_pixel_indices_unused;

    if (IsOccluded(source_depth_image, new_obj_depth_image,
                   &new_pixel_indices_unused,
                   &succ_min_depth,
                   &succ_max_depth)) {
      return -1;
    }

    child_properties->last_min_depth = succ_min_depth;
    child_properties->last_min_depth = succ_max_depth;

    if (!perch_params_.use_color_cost) {
      new_obj_depth_image = ApplyOcclusionMask(new_obj_depth_image,
                                              source_depth_image);
    }

    if (kUseHistogramLazy)
    {
      // Did ICP for occlusion case, need to recompute histogram
      printf("Recomputing histogram score because of occlusion in GetLazyCost()\n");
      double histogram_distance;
      if (!IsValidHistogram(last_object_id, last_cv_obj_color_image, kHistogramLazyScoreThresh, histogram_distance)) {
        printf("Rejecting because of low histogram score in GetLazyCost()\n");
        return -1;
      }
    }

  } else {
    // If there is no occlusion, no need to do ICP, just use the corresponding ICP adjusted state
    // generated at the first level
    int last_idx = child_state.NumObjects() - 1;
    assert(last_idx >= 0);
    assert(adjusted_last_object_state.object_states().size() > 0);

    const auto &last_object = adjusted_last_object_state.object_states().back();
    adjusted_child_state->mutable_object_states()[last_idx] =
      last_object;

    // RGB Aditya
    if (kUseHistogramLazy) {
      printf("Got cached histogram score : %f\n", adjusted_last_object_histogram_score);
      double threshold = kHistogramLazyScoreThresh;
      if (obj_models_[last_object_id].name().compare("pepsi_can") == 0 ||
      obj_models_[last_object_id].name().compare("7up_can") == 0) {
        printf("Pepsi model hack\n");
        threshold = 0.9;
      }
      if (adjusted_last_object_histogram_score > threshold) {
        printf("Rejecting because of low histogram score in GetLazyCost()\n");
        return -1;
      }
    }
    if (perch_params_.use_color_cost) {
      int num_occluders = 0;
      GraphState s = adjusted_last_object_state;
      succ_depth_buffer = GetDepthImage(
        s, &new_obj_depth_image, &new_obj_color_image,
        last_cv_obj_depth_image, last_cv_obj_color_image, &num_occluders, false
      );
    } else {
      new_obj_depth_image = adjusted_last_object_depth_image;
    }

    if (!IsValidPose(source_state, last_object_id,
                     last_object.cont_pose(), true)) {
      std::cout << "Rejecting pose later";

      return -1;
    }

    vector<int> new_pixel_indices_unused;
    unsigned short succ_min_depth_unused, succ_max_depth_unused;

    if (IsOccluded(source_depth_image, new_obj_depth_image,
                   &new_pixel_indices_unused,
                   &succ_min_depth,
                   &succ_max_depth)) {
      return -1;
    }

    child_properties->last_min_depth = succ_min_depth;
    child_properties->last_min_depth = succ_max_depth;

    // std::string imn = "adjusted_last_object_depth_image.png";
    // PrintImage(imn, new_obj_depth_image, true);
    // cv::imshow("valid image", last_cv_obj_color_image);
    // cv::waitKey(100);

  }



  cloud_out = GetGravityAlignedPointCloud(new_obj_depth_image, new_obj_color_image);


  // Compute costs
  // const bool last_level = static_cast<int>(child_state.NumObjects()) ==
  //                         env_params_.num_objects;
  // Must be conservative for lazy;
  const bool last_level = false;


  int target_cost = 0, source_cost = 0, last_level_cost = 0, total_cost = 0;
  target_cost = GetTargetCost(cloud_out);

  vector<int> child_counted_pixels;
  source_cost = GetSourceCost(cloud_out,
                              adjusted_child_state->object_states().back(),
                              last_level, parent_counted_pixels, &child_counted_pixels);

  child_properties->source_cost = source_cost;
  child_properties->target_cost = target_cost;
  child_properties->last_level_cost = 0;

  total_cost = source_cost + target_cost + last_level_cost;
  if (cost_debug_msgs)
    printf("Cost of this state : %d\n", total_cost);

  // std::stringstream cloud_ss;
  // cloud_ss.precision(20);
  // cloud_ss << debug_dir_ + "cloud_" << rand() << ".pcd";
  // pcl::PCDWriter writer;
  // writer.writeBinary (cloud_ss.str()  , *cloud_in);

  // if (image_debug_) {
  //   std::stringstream ss1, ss2, ss3;
  //   ss1.precision(20);
  //   ss2.precision(20);
  //   ss1 << debug_dir_ + "cloud_" << child_id << ".pcd";
  //   ss2 << debug_dir_ + "cloud_aligned_" << child_id << ".pcd";
  //   ss3 << debug_dir_ + "cloud_succ_" << child_id << ".pcd";
  //   pcl::PCDWriter writer;
  //   writer.writeBinary (ss1.str()  , *cloud_in);
  //   writer.writeBinary (ss2.str()  , *cloud_out);
  //   writer.writeBinary (ss3.str()  , *succ_cloud);
  // }
  if (image_debug_) {
    if (IsMaster(mpi_comm_)) {
      // PrintPointCloud(cloud_out, 1, render_point_cloud_topic);
    }
    std::string imn = "new_obj_depth_image_final.png";
    // PrintImage(imn, new_obj_depth_image, true);
    // cv::imshow("valid image", last_cv_obj_color_image);
    // cv::waitKey(100);
  }
  // RGB Aditya
  // *final_depth_image = new_obj_depth_image;
  GetComposedDepthImage(source_depth_image,
                        new_obj_depth_image,
                        final_depth_image);
  return total_cost;
}


int EnvObjectRecognition::GetCost(const GraphState &source_state,
                                  const GraphState &child_state,
                                  const vector<unsigned short> &source_depth_image,
                                  const vector<vector<unsigned char>> &source_color_image,
                                  const vector<int> &parent_counted_pixels, vector<int> *child_counted_pixels,
                                  GraphState *adjusted_child_state, GraphStateProperties *child_properties,
                                  vector<unsigned short> *final_depth_image,
                                  vector<vector<unsigned char>> *final_color_image,
                                  vector<unsigned short> *unadjusted_depth_image,
                                  vector<vector<unsigned char>> *unadjusted_color_image,
                                  double &histogram_score) {
  if (cost_debug_msgs)
    std::cout << "GetCost() : Getting cost for state " << endl;
  assert(child_state.NumObjects() > 0);

  *adjusted_child_state = child_state;
  child_properties->last_max_depth = kKinectMaxDepth;
  child_properties->last_min_depth = 0;

  const auto &last_object = child_state.object_states().back();
  ContPose child_pose = last_object.cont_pose();
  int last_object_id = last_object.id();

  //initializing all containers for images and point clouds
  vector<unsigned short> depth_image, last_obj_depth_image;
  cv::Mat cv_depth_image, last_cv_obj_depth_image;
  vector<vector<unsigned char>> color_image, last_obj_color_image;
  cv::Mat cv_color_image, last_cv_obj_color_image;

  const float *succ_depth_buffer;
  ContPose pose_in(child_pose),
           pose_out(child_pose);
  PointCloudPtr cloud_in(new PointCloud);
  PointCloudPtr succ_cloud(new PointCloud);
  PointCloudPtr cloud_out(new PointCloud);

  // Begin ICP Adjustment
  // Computing images after adding objects to scene
  GraphState s_new_obj;
  s_new_obj.AppendObject(ObjectState(last_object_id,
                                     obj_models_[last_object_id].symmetric(), child_pose));
  succ_depth_buffer = GetDepthImage(s_new_obj, &last_obj_depth_image, &last_obj_color_image,
                                    &last_cv_obj_depth_image, &last_cv_obj_color_image);

  if (kUseHistogramLazy && child_state.NumObjects() == 1)
  // if (kUseHistogramLazy)
  {
    if (!IsValidHistogram(last_object_id, last_cv_obj_color_image, kHistogramLazyScoreThresh, histogram_score)) {
      printf("Rejecting because of histogram from GetCost()\n");
      return -2;
    }
  }

  unadjusted_depth_image->clear();
  unadjusted_color_image->clear();
  GetComposedDepthImage(source_depth_image, source_color_image,
                        last_obj_depth_image, last_obj_color_image,
                        unadjusted_depth_image, unadjusted_color_image);

  unsigned short succ_min_depth_unused, succ_max_depth_unused;
  vector<int> new_pixel_indices;

  if (IsOccluded(source_depth_image, *unadjusted_depth_image, &new_pixel_indices,
                 &succ_min_depth_unused,
                 &succ_max_depth_unused)) {
    // final_depth_image->clear();
    // *final_depth_image = *unadjusted_depth_image;
    if (cost_debug_msgs)
      printf("IsOccluded invalid\n");
    // Can't add new objects that occlude previous ones
    return -1;
  }

  // new_pixel_indices is pixels corresponding to object added in this state
  vector<unsigned short> new_obj_depth_image(kCameraWidth *
                                             kCameraHeight, kKinectMaxDepth);
  vector<unsigned char> color_vector{'0','0','0'};
  vector<vector<unsigned char>> new_obj_color_image(kCameraWidth *
                                             kCameraHeight, color_vector);

  // Do ICP alignment on object *only* if it has been occluded by an existing
  // object in the scene. Otherwise, we could simply use the cached depth image corresponding to the unoccluded ICP adjustement.

  for (size_t ii = 0; ii < new_pixel_indices.size(); ++ii) {
    new_obj_depth_image[new_pixel_indices[ii]] =
      unadjusted_depth_image->at(new_pixel_indices[ii]);

    if (perch_params_.use_color_cost)
      new_obj_color_image[new_pixel_indices[ii]] =
        unadjusted_color_image->at(new_pixel_indices[ii]);
  }

  // Create point cloud (cloud_in) corresponding to new pixels of object that was added in this state.
  cloud_in = GetGravityAlignedPointCloud(new_obj_depth_image, new_obj_color_image);

  if (IsMaster(mpi_comm_)) {
    if (image_debug_) {
      // uint8_t rgb[3] = {0,0,255};
      // PointCloudPtr cloud_icp_in = GetGravityAlignedPointCloud(new_obj_depth_image, rgb);
      // PrintPointCloud(cloud_icp_in, 1, render_point_cloud_topic);
      // PrintPointCloud(cloud_in, 1, render_point_cloud_topic);
    }
  }
  // Align with ICP
  // Only non-occluded points

  GetICPAdjustedPose(cloud_in, pose_in, cloud_out, &pose_out,
                     parent_counted_pixels);

  // GetICPAdjustedPoseCUDA(cloud_in, pose_in, cloud_out, &pose_out,
  //                   parent_counted_pixels);
  // icp_cost = static_cast<int>(kICPCostMultiplier * icp_fitness_score);
  int last_idx = child_state.NumObjects() - 1;

  // TODO: verify
  const ObjectState modified_last_object(last_object.id(),
                                         last_object.symmetric(), pose_out);
  adjusted_child_state->mutable_object_states()[last_idx] = modified_last_object;
  // End ICP Adjustment

  if (cost_debug_msgs) {
  printf("pose before icp");
  std::cout << pose_in << endl;
  printf("pose after icp");
  std::cout << pose_out << endl;
  }

  // std::cout<<adjusted_child_state->object_states().back().cont_pose()<<endl;
  // Check again after icp
  if (!IsValidPose(source_state, last_object_id,
                   adjusted_child_state->object_states().back().cont_pose(), true)) {
    if (cost_debug_msgs)
      printf(" state %d is invalid after icp\n ", source_state);

    // succ_depth_buffer = GetDepthImage(*adjusted_child_state, &depth_image);
    // final_depth_image->clear();
    // *final_depth_image = depth_image;
    // if (source_state.NumObjects() == 0)
    return -1;
    // Aditya uncomment later
  }

  // num_occluders is the number of valid pixels in the input depth image that
  // occlude the rendered scene corresponding to adjusted_child_state.
  int num_occluders = 0;
  cv::Mat cv_depth_image_temp, cv_depth_color_temp;

  if (env_params_.use_external_render == 0)
  {
      succ_depth_buffer = GetDepthImage(*adjusted_child_state, &depth_image, &color_image,
                                          cv_depth_image_temp, cv_depth_color_temp, &num_occluders, false);
  }
  else
  {
      succ_depth_buffer = GetDepthImage(*adjusted_child_state, &depth_image, &color_image,
                                        &cv_depth_image, &cv_color_image);
  }
  // if (kUseHistogramLazy && child_state.NumObjects() == 1)
  // {
  //   if (!IsValidHistogram(cv_depth_color_temp, kHistogramLazyScoreThresh, histogram_score)) {
  //     printf("Rejecting because of histogram from GetCost()\n");
  //     return -2;
  //   }
  // }
  // All points
  succ_cloud = GetGravityAlignedPointCloud(depth_image, color_image);

  unsigned short succ_min_depth, succ_max_depth;
  new_pixel_indices.clear();
  new_obj_depth_image.clear();
  new_obj_depth_image.resize(kNumPixels, kKinectMaxDepth);

  new_obj_color_image.clear();
  new_obj_color_image.resize(kNumPixels, color_vector);

  if (IsOccluded(source_depth_image, depth_image, &new_pixel_indices,
                 &succ_min_depth,
                 &succ_max_depth)) {
    // final_depth_image->clear();
    // *final_depth_image = depth_image;
    printf("IsOccluded invalid\n");

    return -1;
  }

  for (size_t ii = 0; ii < new_pixel_indices.size(); ++ii) {
    new_obj_depth_image[new_pixel_indices[ii]] =
      depth_image[new_pixel_indices[ii]];

    if (perch_params_.use_color_cost)
      new_obj_color_image[new_pixel_indices[ii]] =
        color_image[new_pixel_indices[ii]];
  }

  // Create point cloud (cloud_out) corresponding to new pixels.
  cloud_out = GetGravityAlignedPointCloud(new_obj_depth_image, new_obj_color_image);

  if (IsMaster(mpi_comm_)) {
    if (image_debug_) {
      // uint8_t rgb[3] = {255,0,0};
      // PointCloudPtr cloud_icp_out = GetGravityAlignedPointCloud(new_obj_depth_image, rgb);
      // PrintPointCloud(cloud_icp_out, 1, render_point_cloud_topic);
      // PrintPointCloud(cloud_out, 1, render_point_cloud_topic);
    }
  }
  // Cache the min and max depths
  child_properties->last_min_depth = succ_min_depth;
  child_properties->last_max_depth = succ_max_depth;


  // Compute costs
  using milli = std::chrono::milliseconds;
  auto start = std::chrono::high_resolution_clock::now();

  const bool last_level = static_cast<int>(child_state.NumObjects()) ==
                          env_params_.num_objects;
  int target_cost = 0, source_cost = 0, last_level_cost = 0, total_cost = 0;
  target_cost = GetTargetCost(cloud_out);


  // source_cost = GetSourceCost(succ_cloud,
  //                             adjusted_child_state->object_states().back(),
  //                             last_level, parent_counted_pixels, child_counted_pixels);
  source_cost = GetSourceCost(succ_cloud,
                              adjusted_child_state->object_states().back(),
                              false, parent_counted_pixels, child_counted_pixels);

  // Aditya uncomment
  if (last_level) {
    vector<int> updated_counted_pixels;
    last_level_cost = GetLastLevelCost(succ_cloud,
                                       adjusted_child_state->object_states().back(), *child_counted_pixels,
                                       &updated_counted_pixels);
    // // NOTE: we won't include the points that lie outside the union of volumes.
    // // Refer to the header for documentation on child_counted_pixels.
    // *child_counted_pixels = updated_counted_pixels;
    *child_counted_pixels = updated_counted_pixels;

    // last_level_cost = 0;
    // Aditya remove
  }

  auto finish = std::chrono::high_resolution_clock::now();
  std::cout << "Get Costs() took "
            << std::chrono::duration_cast<milli>(finish - start).count()
            << " milliseconds\n";

  total_cost = source_cost + target_cost + last_level_cost;

  if (perch_params_.use_clutter_mode) {
    total_cost += static_cast<int>(perch_params_.clutter_regularizer * num_occluders);
  }


  // unadjusted_color_image->clear();
  // unadjusted_color_image->shrink_to_fit();
  // unadjusted_depth_image->clear();
  // unadjusted_depth_image->shrink_to_fit();

  *final_depth_image = depth_image;

  // if (source_state.NumObjects() == 0)
  // {
  //   // Store color images in output unit
  //   *final_color_image = color_image;
  // }
  // else
  {
    // Dont store color images in output unit
    unadjusted_color_image->clear();
    unadjusted_color_image->shrink_to_fit();
  }
  // final_color_image->clear();
  // final_color_image->shrink_to_fit();
  // *final_color_image = color_image;

  cloud_out.reset();
  cloud_in.reset();
  succ_cloud.reset();

  child_properties->target_cost = target_cost;
  child_properties->source_cost = source_cost;
  child_properties->last_level_cost = last_level_cost;

  // std::stringstream cloud_ss;
  // cloud_ss.precision(20);
  // cloud_ss << debug_dir_ + "cloud_" << rand() << ".pcd";
  // pcl::PCDWriter writer;
  // writer.writeBinary (cloud_ss.str()  , *succ_cloud);

  if (image_debug_) {
    // cloud_out = GetGravityAlignedPointCloud(*final_depth_image);
    // PrintPointCloud(cloud_out, last_object_id);

    // PrintImage("Test", depth_image);
    // std::stringstream ss1, ss2, ss3;
    // ss1.precision(20);
    // ss2.precision(20);
    // ss1 << debug_dir_ + "cloud_" << child_id << ".pcd";
    // ss2 << debug_dir_ + "cloud_aligned_" << child_id << ".pcd";
    // ss3 << debug_dir_ + "cloud_succ_" << child_id << ".pcd";
    // pcl::PCDWriter writer;
    // writer.writeBinary (ss1.str()  , *cloud_in);
    // writer.writeBinary (ss2.str()  , *cloud_out);
    // writer.writeBinary (ss3.str()  , *succ_cloud);
  }
  if (cost_debug_msgs)
    printf("Cost of this state : %d\n", total_cost);
  return total_cost;
}
int EnvObjectRecognition::GetColorOnlyCost(const GraphState &source_state,
                                  const GraphState &child_state,
                                  const vector<unsigned short> &source_depth_image,
                                  const vector<vector<unsigned char>> &source_color_image,
                                  const vector<int> &parent_counted_pixels, vector<int> *child_counted_pixels,
                                  GraphState *adjusted_child_state, GraphStateProperties *child_properties,
                                  vector<unsigned short> *final_depth_image,
                                  vector<vector<unsigned char>> *final_color_image,
                                  vector<unsigned short> *unadjusted_depth_image,
                                  vector<vector<unsigned char>> *unadjusted_color_image) {
  std::cout << "GetCost() : Getting cost for state " << endl;
  assert(child_state.NumObjects() > 0);

  *adjusted_child_state = child_state;
  child_properties->last_max_depth = kKinectMaxDepth;
  child_properties->last_min_depth = 0;

  const auto &last_object = child_state.object_states().back();
  ContPose child_pose = last_object.cont_pose();
  int last_object_id = last_object.id();

  //initializing all containers for images and point clouds
  vector<unsigned short> depth_image, last_obj_depth_image;
  cv::Mat cv_depth_image, last_cv_obj_depth_image;
  vector<vector<unsigned char>> color_image, last_obj_color_image;
  cv::Mat cv_color_image, last_cv_obj_color_image;

  const float *succ_depth_buffer;
  ContPose pose_in(child_pose),
           pose_out(child_pose);
  // PointCloudPtr cloud_in(new PointCloud);
  // PointCloudPtr succ_cloud(new PointCloud);
  // PointCloudPtr cloud_out(new PointCloud);

  // Begin ICP Adjustment
  // Computing images after adding objects to scene
  GraphState s_new_obj;
  s_new_obj.AppendObject(ObjectState(last_object_id,
                                     obj_models_[last_object_id].symmetric(), child_pose));
  succ_depth_buffer = GetDepthImage(s_new_obj, &last_obj_depth_image, &last_obj_color_image,
                                    &last_cv_obj_depth_image, &last_cv_obj_color_image);

  unadjusted_depth_image->clear();
  unadjusted_color_image->clear();
  GetComposedDepthImage(source_depth_image, source_color_image,
                        last_obj_depth_image, last_obj_color_image,
                        unadjusted_depth_image, unadjusted_color_image);

  unsigned short succ_min_depth_unused, succ_max_depth_unused;
  vector<int> new_pixel_indices;

  if (IsOccluded(source_depth_image, *unadjusted_depth_image, &new_pixel_indices,
                 &succ_min_depth_unused,
                 &succ_max_depth_unused)) {
    // final_depth_image->clear();
    // *final_depth_image = *unadjusted_depth_image;
    printf("IsOccluded invalid\n");
    // Can't add new objects that occlude previous ones
    return -1;
  }

  
  //child_properties->last_min_depth = succ_min_depth;
  //child_properties->last_max_depth = succ_max_depth;

  // Compute costs
  const bool last_level = static_cast<int>(child_state.NumObjects()) ==
                          env_params_.num_objects;
  int target_cost = 0, source_cost = 0, last_level_cost = 0, total_cost = 0;
  auto start = chrono::steady_clock::now();
  target_cost = GetColorCost(&last_cv_obj_depth_image, &last_cv_obj_color_image);
  auto end = chrono::steady_clock::now();
  std::cout<< "time for the cost: " << chrono::duration_cast<chrono::milliseconds>(end - start).count() << std::endl;
  // source_cost = GetSourceCost(succ_cloud,
  //                             adjusted_child_state->object_states().back(),
  //                             last_level, parent_counted_pixels, child_counted_pixels);
  source_cost = 0;

  // Aditya uncomment 
  if (last_level) {
    vector<int> updated_counted_pixels;
    last_level_cost = 0;
    // // NOTE: we won't include the points that lie outside the union of volumes.
    // // Refer to the header for documentation on child_counted_pixels.
    // *child_counted_pixels = updated_counted_pixels;
    *child_counted_pixels = updated_counted_pixels;

    // last_level_cost = 0;
    // Aditya remove
  }

  total_cost = target_cost;

  unadjusted_color_image->clear();
  unadjusted_color_image->shrink_to_fit();
  // unadjusted_depth_image->clear();
  // unadjusted_depth_image->shrink_to_fit();

  *final_depth_image = last_obj_depth_image;

  // final_color_image->clear();
  // final_color_image->shrink_to_fit();
  // *final_color_image = color_image;

  // cloud_out.reset();
  // cloud_in.reset();
  // succ_cloud.reset();

  child_properties->target_cost = target_cost;
  child_properties->source_cost = source_cost;
  child_properties->last_level_cost = last_level_cost;

  // std::stringstream cloud_ss;
  // cloud_ss.precision(20);
  // cloud_ss << debug_dir_ + "cloud_" << rand() << ".pcd";
  // pcl::PCDWriter writer;
  // writer.writeBinary (cloud_ss.str()  , *succ_cloud);

  if (image_debug_) {
    // cloud_out = GetGravityAlignedPointCloud(*final_depth_image);
    // PrintPointCloud(cloud_out, last_object_id);

    // PrintImage("Test", depth_image);
    // std::stringstream ss1, ss2, ss3;
    // ss1.precision(20);
    // ss2.precision(20);
    // ss1 << debug_dir_ + "cloud_" << child_id << ".pcd";
    // ss2 << debug_dir_ + "cloud_aligned_" << child_id << ".pcd";
    // ss3 << debug_dir_ + "cloud_succ_" << child_id << ".pcd";
    // pcl::PCDWriter writer;
    // writer.writeBinary (ss1.str()  , *cloud_in);
    // writer.writeBinary (ss2.str()  , *cloud_out);
    // writer.writeBinary (ss3.str()  , *succ_cloud);
  }
  printf("Cost of this state : %d\n", total_cost);
  return total_cost;

  
}
bool EnvObjectRecognition::IsOccluded(const vector<unsigned short>
                                      &parent_depth_image, const vector<unsigned short> &succ_depth_image,
                                      vector<int> *new_pixel_indices, unsigned short *min_succ_depth,
                                      unsigned short *max_succ_depth) {

  assert(static_cast<int>(parent_depth_image.size()) == kNumPixels);
  assert(static_cast<int>(succ_depth_image.size()) == kNumPixels);

  new_pixel_indices->clear();
  *min_succ_depth = kKinectMaxDepth;
  *max_succ_depth = 0;

  bool is_occluded = false;

  for (int jj = 0; jj < kNumPixels; ++jj) {

    if (succ_depth_image[jj] != kKinectMaxDepth &&
        parent_depth_image[jj] == kKinectMaxDepth) {
      new_pixel_indices->push_back(jj);

      // Find mininum depth of new pixels
      if (succ_depth_image[jj] != kKinectMaxDepth &&
          succ_depth_image[jj] < *min_succ_depth) {
        *min_succ_depth = succ_depth_image[jj];
      }

      // Find maximum depth of new pixels
      if (succ_depth_image[jj] != kKinectMaxDepth &&
          succ_depth_image[jj] > *max_succ_depth) {
        *max_succ_depth = succ_depth_image[jj];
      }
    }

    // Occlusion
    if (succ_depth_image[jj] != kKinectMaxDepth &&
        parent_depth_image[jj] != kKinectMaxDepth &&
        succ_depth_image[jj] < parent_depth_image[jj]) {
      is_occluded = true;
      break;
    }

    // if (succ_depth_image[jj] == kKinectMaxDepth && observed_depth_image_[jj] != kKinectMaxDepth) {
    //   obs_pixels.push_back(jj);
    // }
  }

  if (is_occluded) {
    new_pixel_indices->clear();
    *min_succ_depth = kKinectMaxDepth;
    *max_succ_depth = 0;
  }

  return is_occluded;
}
double EnvObjectRecognition::getColorDistanceCMC(uint32_t rgb_1, uint32_t rgb_2) const
{
    uint8_t r = (rgb_1 >> 16);
    uint8_t g = (rgb_1 >> 8);
    uint8_t b = (rgb_1);
    ColorSpace::Rgb point_1_color(r, g, b);
    // printf("observed_color point color: %d,%d,%d\n", r,g,b);

    r = (rgb_2 >> 16);
    g = (rgb_2 >> 8);
    b = (rgb_2);
    // printf("model point color: %d,%d,%d\n", r,g,b);
    ColorSpace::Rgb point_2_color(r, g, b);

    double color_distance =
              ColorSpace::CmcComparison::Compare(&point_1_color, &point_2_color);

    return color_distance;

}
double EnvObjectRecognition::getColorDistance(uint32_t rgb_1, uint32_t rgb_2) const
{
    uint8_t r = (rgb_1 >> 16);
    uint8_t g = (rgb_1 >> 8);
    uint8_t b = (rgb_1);
    ColorSpace::Rgb point_1_color(r, g, b);
    // printf("observed_color point color: %d,%d,%d\n", r,g,b);

    r = (rgb_2 >> 16);
    g = (rgb_2 >> 8);
    b = (rgb_2);
    // printf("model point color: %d,%d,%d\n", r,g,b);
    ColorSpace::Rgb point_2_color(r, g, b);

    double color_distance =
              ColorSpace::Cie2000Comparison::Compare(&point_1_color, &point_2_color);

    return color_distance;

}
double EnvObjectRecognition::getColorDistance(uint8_t r1,uint8_t g1,uint8_t b1,uint8_t r2,uint8_t g2,uint8_t b2) const
{
    ColorSpace::Rgb point_1_color(r1, g1, b1);
    ColorSpace::Rgb point_2_color(r2, g2, b2);

    double color_distance =
              ColorSpace::Cie2000Comparison::Compare(&point_1_color, &point_2_color);

    return color_distance;

}

int EnvObjectRecognition::getNumColorNeighboursCMC(PointT point,
                                              const PointCloudPtr point_cloud) const
{
    uint32_t rgb_1 = *reinterpret_cast<int*>(&point.rgb);
    // int num_color_neighbors_found = 0;
    for (size_t i = 0; i < point_cloud->points.size(); i++)
    {
        // Find color matching points in observed_color point cloud
        uint32_t rgb_2 = *reinterpret_cast<int*>(&point_cloud->points[i].rgb);
        double color_distance = getColorDistanceCMC(rgb_1, rgb_2);
        if (color_distance < kColorDistanceThresholdCMC) {
          // If color is close then this is a valid neighbour
          // num_color_neighbors_found++;
          return 1;
        }
    }
    return 0;
}

int EnvObjectRecognition::getNumColorNeighbours(PointT point,
                                              vector<int> indices,
                                              const PointCloudPtr point_cloud) const
{
    uint32_t rgb_1 = *reinterpret_cast<int*>(&point.rgb);
    int num_color_neighbors_found = 0;
    for (int i = 0; i < indices.size(); i++)
    {
        // Find color matching points in observed_color point cloud
        uint32_t rgb_2 = *reinterpret_cast<int*>(&point_cloud->points[indices[i]].rgb);
        double color_distance = getColorDistance(rgb_1, rgb_2);
        if (color_distance < perch_params_.color_distance_threshold) {
          // If color is close then this is a valid neighbour
          num_color_neighbors_found++;
        }
    }
    return num_color_neighbors_found;
}
int EnvObjectRecognition::GetColorCost(cv::Mat *cv_depth_image,cv::Mat *cv_color_image) {
  
    int cost = 0;
    // cv::Mat lab;
    // cv::Mat lab1;
    // cv::cvtColor(*cv_color_image,lab,cv::COLOR_BGR2Lab);
    // cv::cvtColor(cv_input_color_image,lab1,cv::COLOR_BGR2Lab);
    // cv::Mat destiny = cv::Mat::zeros( cv_color_image->size(), CV_8UC1);
    
    // difffilter(lab,lab1,destiny);
    // cost = sum(destiny)[0];
    /*cv::Mat img(540, 960,CV_64F);
    cv::Vec3b pixel1;
    cv::Vec3b pixel2;
    cv::Vec3b pixel_depth;
    uint8_t r1;
    uint8_t g1;
    uint8_t b1;
    uint8_t r2;
    uint8_t g2;
    uint8_t b2;
    uint8_t depth;
    int row;
    int col;
    double cur_dist;
    bool valid;
    for(int r = 0; r < cv_input_color_image.rows; ++r) {
        for(int c = 0; c < cv_input_color_image.cols; ++c) {
          valid = false;
          pixel1 = cv_input_color_image.at<cv::Vec3b>(r,c);
          
          pixel_depth = cv_depth_image->at<cv::Vec3b>(r,c);
          r1 = (uint8_t)pixel1[0];
          g1 = (uint8_t)pixel1[1];
          b1 = (uint8_t)pixel1[2];
          
          depth = (uint8_t)pixel_depth[0];
          for(int i = -2; i <3;i++){
            row = r+i;
            col = c+i;
            if(row >= 0 && row <cv_input_color_image.rows && col >= 0 && col <cv_input_color_image.cols){
              pixel2 = cv_color_image->at<cv::Vec3b>(r,c);
              r2 = (uint8_t)pixel2[0];
              g2 = (uint8_t)pixel2[1];
              b2 = (uint8_t)pixel2[2];
              cur_dist = getColorDistance(r1,g1,b1,r2,g2,b2);
              if(cur_dist < perch_params_.color_distance_threshold){
                valid = true;
              }
            }
          }
          // if(r2 != 0 || g2 != 0 || b2 != 0){
          //   std::cout<< r << ":"<< c << std::endl;
          //   std::cout<<cur_dist<<std::endl;
          //   //std::cout<< unsigned(r2) << ","<< unsigned(g2) << ","<< unsigned(b2) << std::endl;
          // }
          
          if(valid == false){
            // cost += depth*depth;
            img.at<double>(r,c) = cur_dist;
            cost += 1;
          }
            
        }
    }*/
    // std::stringstream ssc1;
    // ssc1.precision(20);
    // ssc1 << debug_dir_ << "input"<< cost <<".png";;
    // std::stringstream ssc2;
    // ssc2.precision(20);
    // ssc2 << debug_dir_ << "generated"<< cost <<".png";;
    // cv::imwrite(ssc1.str().c_str(), cv_input_color_image);
    // cv::imwrite(ssc2.str().c_str(), *cv_color_image);
    // std::stringstream color_mismatch;
    // color_mismatch.precision(20);
    // color_mismatch << debug_dir_ << "mismatch"<< cost <<".png";
    // cv::imwrite(color_mismatch.str().c_str(), img);
    // std::cout << "M = "<< std::endl << " "  << img << std::endl << std::endl;
    std::cout<< "cost !!!!!!!!!!!!!!!!!!!!"<< cost <<std::endl;
    return cost;
}

int EnvObjectRecognition::GetTargetCost(const PointCloudPtr
                                        partial_rendered_cloud) {
  // Nearest-neighbor cost
  if (IsMaster(mpi_comm_)) {
    if (image_debug_) {
      // PrintPointCloud(partial_rendered_cloud, 1, render_point_cloud_topic);
    }
  }
  double nn_score = 0;
  double nn_color_score = 0;
  int total_color_neighbours = 0;
  // Searching in observed_color cloud for every point in rendered cloud
  using milli = std::chrono::milliseconds;
  auto start = std::chrono::high_resolution_clock::now();
  if (cost_debug_msgs)
    printf("GetTargetCost()\n");

  for (size_t ii = 0; ii < partial_rendered_cloud->points.size(); ++ii) {
    // A rendered point will get cost as 1 if there are no points in neighbourhood or if
    // neighbourhoold points dont match the color of the point
    vector<int> indices;
    vector<float> sqr_dists;
    PointT point = partial_rendered_cloud->points[ii];
    // std::cout<<point<<endl;
    // Search neighbours in observed_color point cloud
    int num_neighbors_found = knn->radiusSearch(point,
                                                perch_params_.sensor_resolution,
                                                indices,
                                                sqr_dists, 1);
    const bool point_unexplained = num_neighbors_found == 0;


    double cost = 0;
    double color_cost = 0;
    if (point_unexplained) {
      // If no neighbours then cost is high
      if (kUseDepthSensitiveCost) {
        auto camera_origin = env_params_.camera_pose.translation();
        PointT camera_origin_point;
        camera_origin_point.x = camera_origin[0];
        camera_origin_point.y = camera_origin[1];
        camera_origin_point.z = camera_origin[2];
        double range = pcl::euclideanDistance(camera_origin_point, point);
        cost = kDepthSensitiveCostMultiplier * range;
      } else {
        cost = 1.0;
      }
    } else {
      // Check RGB cost here, if RGB explained then cost is 0 else reassign to 1
      if (env_params_.use_external_render == 1 || perch_params_.use_color_cost)
      {
        int num_color_neighbors_found =
            getNumColorNeighbours(point, indices, observed_cloud_);
        total_color_neighbours += num_color_neighbors_found;
        if (num_color_neighbors_found == 0) {
          // If no color neighbours found then cost is 1.0
          color_cost = 1.0;
          cost = 1.0;
        } else {
          color_cost = 0.0;
          cost = 0.0;
        }
      }
      else {
        cost = 0.0;
      }
    }

    nn_score += cost;
    nn_color_score += color_cost;
  }

  // distance score might be low but rgb score can still be high
  // color score tells how many points which were depth explained mismatched
  if (cost_debug_msgs)
    printf("Color score for this state : %f with distance score : %f, color neighbours : %d\n", nn_color_score, nn_score, total_color_neighbours);

  int target_cost = 0;
  int target_color_cost = 0;

  if (kNormalizeCost) {
    // if (partial_rendered_cloud->points.empty()) {
    if (static_cast<int>(partial_rendered_cloud->points.size()) <
        perch_params_.min_neighbor_points_for_valid_pose) {
      return 100;
    }

    target_cost = static_cast<int>(nn_score * kNormalizeCostBase /
                                   partial_rendered_cloud->points.size());
  } else {
    target_cost = static_cast<int>(nn_score);
  }
  auto finish = std::chrono::high_resolution_clock::now();
  if (cost_debug_msgs)
    std::cout << "GetTargetCost() took "
              << std::chrono::duration_cast<milli>(finish - start).count()
              << " milliseconds\n";
  return target_cost;
}

int EnvObjectRecognition::GetSourceCost(const PointCloudPtr
                                        full_rendered_cloud, const ObjectState &last_object, const bool last_level,
                                        const std::vector<int> &parent_counted_pixels,
                                        std::vector<int> *child_counted_pixels) {

  //TODO: TESTING
  assert(!last_level);

  // Compute the cost of points made infeasible in the observed_color point cloud.
  pcl::search::KdTree<PointT>::Ptr knn_reverse;
  knn_reverse.reset(new pcl::search::KdTree<PointT>(true));
  knn_reverse->setInputCloud(full_rendered_cloud);

  child_counted_pixels->clear();
  *child_counted_pixels = parent_counted_pixels;

  // TODO: make principled
  if (full_rendered_cloud->points.empty()) {
    if (kNormalizeCost) {
      return 100;
    }

    return 100000;
  }

  std::sort(child_counted_pixels->begin(), child_counted_pixels->end());

  vector<int> indices_to_consider;
  // Indices of points within the circumscribed cylinder, but outside the
  // inscribed cylinder.
  vector<int> indices_circumscribed;

  if (last_level) {
    indices_to_consider.resize(valid_indices_.size());
    auto it = std::set_difference(valid_indices_.begin(), valid_indices_.end(),
                                  child_counted_pixels->begin(), child_counted_pixels->end(),
                                  indices_to_consider.begin());
    indices_to_consider.resize(it - indices_to_consider.begin());
  } else {
    ContPose last_obj_pose = last_object.cont_pose();
    int last_obj_id = last_object.id();
    PointT obj_center;
    obj_center.x = last_obj_pose.x();
    obj_center.y = last_obj_pose.y();
    obj_center.z = last_obj_pose.z();

    vector<float> sqr_dists;
    vector<int> validation_points;

    // Check that this object state is valid, i.e, it has at least
    // perch_params_.min_neighbor_points_for_valid_pose within the circumscribed cylinder.
    // This should be true if we correctly validate successors.

    int num_validation_neighbors;
    if (env_params_.use_external_pose_list == 1)
    {
      const double validation_search_rad =
        obj_models_[last_obj_id].GetInflationFactor() *
        obj_models_[last_obj_id].GetCircumscribedRadius3D();
      num_validation_neighbors = knn->radiusSearch(obj_center,
                                                    validation_search_rad,
                                                    validation_points,
                                                    sqr_dists, kNumPixels);
      printf("validation_search_rad : %f, num_validation_neighbors: %d\n", validation_search_rad, num_validation_neighbors);
      
    }
    else
    {
      const double validation_search_rad =
        obj_models_[last_obj_id].GetInflationFactor() *
        obj_models_[last_obj_id].GetCircumscribedRadius();
      num_validation_neighbors = projected_knn_->radiusSearch(obj_center,
                                                    validation_search_rad,
                                                    validation_points,
                                                    sqr_dists, kNumPixels);
    }

    assert(num_validation_neighbors >=
           perch_params_.min_neighbor_points_for_valid_pose);

    // Remove the points already counted.
    vector<int> filtered_validation_points;
    filtered_validation_points.resize(validation_points.size());
    std::sort(validation_points.begin(), validation_points.end());
    auto filter_it = std::set_difference(validation_points.begin(),
                                         validation_points.end(),
                                         child_counted_pixels->begin(), child_counted_pixels->end(),
                                         filtered_validation_points.begin());
    filtered_validation_points.resize(filter_it -
                                      filtered_validation_points.begin());
    validation_points = filtered_validation_points;

    vector<Eigen::Vector3d> eig_points(validation_points.size());
    vector<Eigen::Vector2d> eig2d_points(validation_points.size());

    // Points of observed_color cloud not counted
    for (size_t ii = 0; ii < validation_points.size(); ++ii) {
      PointT point = observed_cloud_->points[validation_points[ii]];
      eig_points[ii][0] = point.x;
      eig_points[ii][1] = point.y;
      eig_points[ii][2] = point.z;

      eig2d_points[ii][0] = point.x;
      eig2d_points[ii][1] = point.y;
    }

    // From not counted points find which ones are inside the cylinder of object
    vector<bool> inside_points;
    if (env_params_.use_external_pose_list == 1) {
      inside_points = obj_models_[last_obj_id].PointsInsideMesh(
                                    eig_points, last_object.cont_pose());
    } else {
      inside_points = obj_models_[last_obj_id].PointsInsideFootprint(
                                    eig2d_points, last_object.cont_pose());
    }

    indices_to_consider.clear();

    for (size_t ii = 0; ii < inside_points.size(); ++ii) {
      if (inside_points[ii]) {
        indices_to_consider.push_back(validation_points[ii]);
      }
    }

    if (cost_debug_msgs)
      printf("Points inside: %zu, total points: %zu\n", indices_to_consider.size(), eig_points.size());

    // The points within the inscribed cylinder are the ones made
    // "infeasible".
    //   const double inscribed_rad = obj_models_[last_obj_id].GetInscribedRadius();
    //   const double inscribed_rad_sq = inscribed_rad * inscribed_rad;
    //   vector<int> infeasible_points;
    //   infeasible_points.reserve(validation_points.size());
    //
    //   for (size_t ii = 0; ii < validation_points.size(); ++ii) {
    //     if (sqr_dists[ii] <= inscribed_rad_sq) {
    //       infeasible_points.push_back(validation_points[ii]);
    //     } else {
    //       indices_circumscribed.push_back(validation_points[ii]);
    //     }
    //   }
    //   indices_to_consider = infeasible_points;
  }

  double nn_score = 0.0;

  for (const int ii : indices_to_consider) {
    child_counted_pixels->push_back(ii);

    PointT point = observed_cloud_->points[ii];
    vector<float> sqr_dists;
    vector<int> indices;
    int num_neighbors_found = knn_reverse->radiusSearch(point,
                                                        perch_params_.sensor_resolution,
                                                        indices,
                                                        sqr_dists, 1);
    bool point_unexplained = num_neighbors_found == 0;

    if (point_unexplained) {
      if (kUseDepthSensitiveCost) {
        auto camera_origin = env_params_.camera_pose.translation();
        PointT camera_origin_point;
        camera_origin_point.x = camera_origin[0];
        camera_origin_point.y = camera_origin[1];
        camera_origin_point.z = camera_origin[2];
        double range = pcl::euclideanDistance(camera_origin_point, point);
        nn_score += kDepthSensitiveCostMultiplier * range;
      } else {
        nn_score += 1.0;
      }
    }
    else
    {
        // Check RGB cost
        if (env_params_.use_external_render == 1 || perch_params_.use_color_cost)
        {
            int num_color_neighbors_found =
                getNumColorNeighbours(point, indices, full_rendered_cloud);

            if (num_color_neighbors_found == 0) {
                // If no color neighbours found then cost is 1.0
                nn_score += 1.0;
            }
        }
    }
  }

  // Every point within the circumscribed cylinder that has been explained by a
  // rendered point can be considered accounted for.
  // TODO: implement a point within mesh method.
  // int num_valid = 0;
  // for (const int ii : indices_circumscribed) {
  //   PointT point = observed_cloud_->points[ii];
  //   vector<float> sqr_dists;
  //   vector<int> indices;
  //   int num_neighbors_found = knn_reverse->radiusSearch(point,
  //                                                       perch_params_.sensor_resolution,
  //                                                       indices,
  //                                                       sqr_dists, 1);
  //   bool point_unexplained = num_neighbors_found == 0;
  //   if (!point_unexplained) {
  //     child_counted_pixels->push_back(ii);
  //     ++num_valid;
  //   }
  // }

  // Counted pixels always need to be sorted.
  std::sort(child_counted_pixels->begin(), child_counted_pixels->end());

  int source_cost = 0;

  if (kNormalizeCost) {
    // if (indices_to_consider.empty()) {
    if (static_cast<int>(indices_to_consider.size()) <
        perch_params_.min_neighbor_points_for_valid_pose) {
      return 100;
    }

    source_cost = static_cast<int>(nn_score * kNormalizeCostBase / indices_to_consider.size());
  } else {
    source_cost = static_cast<int>(nn_score);
  }

  return source_cost;
}



int EnvObjectRecognition::GetLastLevelCost(const PointCloudPtr
                                           full_rendered_cloud,
                                           const ObjectState &last_object,
                                           const std::vector<int> &counted_pixels,
                                           std::vector<int> *updated_counted_pixels) {
  // There is no residual cost when we operate with the clutter mode.
  if (perch_params_.use_clutter_mode) {
    return 0;
  }

  // Compute the cost of points made infeasible in the observed_color point cloud.
  pcl::search::KdTree<PointT>::Ptr knn_reverse;
  knn_reverse.reset(new pcl::search::KdTree<PointT>(true));
  knn_reverse->setInputCloud(full_rendered_cloud);

  updated_counted_pixels->clear();
  *updated_counted_pixels = counted_pixels;

  // TODO: make principled
  if (full_rendered_cloud->points.empty()) {
    return 100000;
  }

  std::sort(updated_counted_pixels->begin(), updated_counted_pixels->end());

  vector<int> indices_to_consider;

  indices_to_consider.resize(valid_indices_.size());
  auto it = std::set_difference(valid_indices_.begin(), valid_indices_.end(),
                                updated_counted_pixels->begin(), updated_counted_pixels->end(),
                                indices_to_consider.begin());
  indices_to_consider.resize(it - indices_to_consider.begin());

  double nn_score = 0.0;

  for (const int ii : indices_to_consider) {
    updated_counted_pixels->push_back(ii);

    PointT point = observed_cloud_->points[ii];
    vector<float> sqr_dists;
    vector<int> indices;
    int num_neighbors_found = knn_reverse->radiusSearch(point,
                                                        perch_params_.sensor_resolution,
                                                        indices,
                                                        sqr_dists, 1);
    bool point_unexplained = num_neighbors_found == 0;

    if (point_unexplained) {
      if (kUseDepthSensitiveCost) {
        auto camera_origin = env_params_.camera_pose.translation();
        PointT camera_origin_point;
        camera_origin_point.x = camera_origin[0];
        camera_origin_point.y = camera_origin[1];
        camera_origin_point.z = camera_origin[2];
        double range = pcl::euclideanDistance(camera_origin_point, point);
        nn_score += kDepthSensitiveCostMultiplier * range;
      } else {
        nn_score += 1.0;
      }
    }
    // else
    // {
    //     // Check RGB cost, not used, no need to check rgb cost at last level
    //     if (env_params_.use_external_render == 1)
    //     {
    //         int num_color_neighbors_found =
    //             getNumColorNeighbours(point, indices, full_rendered_cloud);

    //         if (num_color_neighbors_found == 0) {
    //             // If no color neighbours found then cost is 1.0
    //             nn_score += 1.0;
    //         }
    //     }
    // }
  }

  assert(updated_counted_pixels->size() == valid_indices_.size());

  int last_level_cost = static_cast<int>(nn_score);
  return last_level_cost;
}


void EnvObjectRecognition::getGlobalPointCV (int u, int v, float range,
                                                  const Eigen::Isometry3d &pose, 
                                                  Eigen::Vector3f &world_point) {
  pcl::PointXYZ p;
  // range = -range;

  float camera_fx_reciprocal_ = 1.0f / kCameraFX;
  float camera_fy_reciprocal_ = 1.0f / kCameraFY;

  p.z = range;
  p.x = (static_cast<float> (u) - kCameraCX) * range * (camera_fx_reciprocal_);
  p.y = (static_cast<float> (v) - kCameraCY) * range * (camera_fy_reciprocal_);

  // Eigen::Matrix4f T;
  // T <<  0, 0, -1, 0,
  // -1, 0,  0, 0,
  // 0, 1,  0, 0,
  // 0, 0,  0, 1;
  Eigen::Matrix4f transform = pose.matrix ().cast<float> ();
  // transform = transform * T;
  Eigen::Matrix<float, 3, 1> pt (p.x, p.y, p.z);

  world_point = transform.block<3,3>(0,0) * pt;
  world_point += transform.block<3,1>(0,3);

}

PointCloudPtr EnvObjectRecognition::GetGravityAlignedPointCloudCV(
  cv::Mat depth_image, cv::Mat color_image, cv::Mat predicted_mask_image, double depth_factor) {

  auto start = std::chrono::high_resolution_clock::now();
  PointCloudPtr cloud(new PointCloud);
  // double min;
  // double max;
  // cv::minMaxIdx(depth_image, &min, &max);
  // printf("min:%f, max:%f", min, max);
  // cv::Mat adjMap;
  // depth_image.convertTo(adjMap, CV_16U, 65535 / (max-min), -min);

  printf("GetGravityAlignedPointCloudCV()\n");
  cv::Size s = depth_image.size();
  cv::Mat filtered_depth_image(s.height, s.width, CV_32SC1, cv::Scalar(0));
  cv::Mat unfiltered_depth_image(s.height, s.width, CV_32SC1, cv::Scalar(0));
  // cv::Mat filtered_color_image = cv::Mat(s.height, s.width, CV_8UC3, cv::Scalar(0,0,0));
  vector<uint8_t> r_vec(s.height * s.width, 0);
  vector<uint8_t> g_vec(s.height * s.width, 0);
  vector<uint8_t> b_vec(s.height * s.width, 0);
  std::cout << "depth_image size " << s << endl;
  // std::cout<<cam_to_world_.matrix()<<endl;
  Eigen::Isometry3d transform;
  if (env_params_.use_external_render == 1) {
    transform = cam_to_world_ ;
  }
  else {
    // Rotate camera frame by 90 to optical frame because we are creating point cloud from images
    Eigen::Isometry3d cam_to_body;
    cam_to_body.matrix() << 0, 0, 1, 0,
                      -1, 0, 0, 0,
                      0, -1, 0, 0,
                      0, 0, 0, 1;
    transform = cam_to_world_ * cam_to_body;
  }
  for (int u = 0; u < s.width; u++) {
    for (int v = 0; v < s.height; v++) {
      vector<int> indices;
      vector<float> sqr_dists;
      pcl::PointXYZRGB point;

      // https://stackoverflow.com/questions/8932893/accessing-certain-pixel-rgb-value-in-opencv
      cv::Vec3b cv_vec = color_image.at<cv::Vec3b>(v,u);
      uint32_t rgbc = ((uint32_t)color_image.at<cv::Vec3b>(v,u)[2] << 16 | (uint32_t)color_image.at<cv::Vec3b>(v,u)[1]<< 8 | (uint32_t)color_image.at<cv::Vec3b>(v,u)[0]);
      point.rgb = *reinterpret_cast<float*>(&rgbc);
      // printf("depth data : %f\n", static_cast<float>(depth_image.at<uchar>(v,u)));
      // 255.0 * 10.0
      Eigen::Vector3f point_eig;
      if (env_params_.use_external_pose_list == 1)
      {
        // When using FAT dataset with model
        getGlobalPointCV(u, v,
                              static_cast<float>(depth_image.at<unsigned short>(v,u)/depth_factor), transform,
                              point_eig);
      }
      else {
        getGlobalPointCV(u, v,
                              static_cast<float>(depth_image.at<uchar>(v,u)/depth_factor), transform,
                              point_eig);
        // printf("depth data : %d\n", (depth_image.at<int>(v,u)));
        // printf("depth data : %d\n", (depth_image.at<uchar>(v,u)));
        // printf("depth data : %f\n", static_cast<float>(depth_image.at<unsigned short>(v,u) - min)/1000);
        // printf("depth data : %f\n", static_cast<float>(depth_image.at<unsigned short>(v,u)*65535.0 / (max-min) - min));
        // kinect_simulator_->rl_->getGlobalPointCV(u, v,
        //                                   static_cast<float>(depth_image.at<unsigned short>(v,u)*65535.0 / (max-min) - min)/1000000, transform,
        //                                   point_eig);
      }

      point.x = point_eig[0];
      point.y = point_eig[1];
      point.z = point_eig[2];

      if (env_params_.use_external_pose_list == 0)
      {
        unfiltered_depth_image.at<int32_t>(v,u) = static_cast<int32_t>(depth_image.at<uchar>(v,u));
        if (point.z >= env_params_.table_height
            && point.x <= env_params_.x_max && point.x >= env_params_.x_min
            && point.y <= env_params_.y_max && point.y >= env_params_.y_min)
          {
            cloud->points.push_back(point);
            filtered_depth_image.at<int32_t>(v,u) = static_cast<int32_t>(depth_image.at<uchar>(v,u));
            // filtered_color_image.at<cv::Vec3b>(v,u) = color_image.at<cv::Vec3b>(v,u);
            r_vec[u + v * s.width] = static_cast<uchar>(cv_vec[2]);
            g_vec[u + v * s.width] = static_cast<uchar>(cv_vec[1]);
            b_vec[u + v * s.width] = static_cast<uchar>(cv_vec[0]);
            // if (static_cast<int32_t>(depth_image.at<uchar>(v,u)/255) > 0)
            // printf("%d\n", static_cast<int32_t>(depth_image.at<uchar>(v,u)/255));
          }
      }
      else
      {
        // printf("%d\n", predicted_mask_image.at<uchar>(v, u));
        unfiltered_depth_image.at<int32_t>(v,u) = static_cast<int32_t>(depth_image.at<unsigned short>(v,u));
        if (predicted_mask_image.at<uchar>(v, u) > 0)
        {
          cloud->points.push_back(point);
          int label_mask_i = predicted_mask_image.at<uchar>(v, u);

          // Store point cloud corresponding to this object, will be used for label specific icp
          // segmented_object_clouds[label_mask_i-1]->points.push_back(point);
          filtered_depth_image.at<int32_t>(v,u) = static_cast<int32_t>(depth_image.at<unsigned short>(v,u));
          r_vec[u + v * s.width] = static_cast<uchar>(cv_vec[2]);
          g_vec[u + v * s.width] = static_cast<uchar>(cv_vec[1]);
          b_vec[u + v * s.width] = static_cast<uchar>(cv_vec[0]);
        }
      }
    }
  }
  // cv::imwrite("test_filter_depth.png", filtered_depth_image);
  cv_input_filtered_depth_image = filtered_depth_image;
  // cv_input_filtered_color_image = filtered_color_image;
  cv_input_unfiltered_depth_image = unfiltered_depth_image;
  cv_input_filtered_color_image_vec.push_back(r_vec);
  cv_input_filtered_color_image_vec.push_back(g_vec);
  cv_input_filtered_color_image_vec.push_back(b_vec);
  // cv::imwrite("test_filter_color.png", filtered_color_image);
  cloud->width = 1;
  cloud->height = cloud->points.size();
  cloud->is_dense = false;

  auto finish = std::chrono::high_resolution_clock::now();
  
  printf("GetGravityAlignedPointCloudCV() done in %d milliseconds\n",  
    std::chrono::duration_cast<std::chrono::milliseconds>(finish - start).count());

  return cloud;
}

PointCloudPtr EnvObjectRecognition::GetGravityAlignedPointCloudCV(
  cv::Mat depth_image, cv::Mat color_image, double depth_factor) {

  PointCloudPtr cloud(new PointCloud);
  // double min;
  // double max;
  // cv::minMaxIdx(depth_image, &min, &max);
  // printf("min:%f, max:%f", min, max);
  // cv::Mat adjMap;
  // depth_image.convertTo(adjMap, CV_16U, 65535 / (max-min), -min);

  printf("GetGravityAlignedPointCloudCV()\n");
  cv::Size s = depth_image.size();
  // std::cout << "depth_image size " << s << endl;
  // std::cout<<cam_to_world_.matrix()<<endl;
  Eigen::Isometry3d transform;
  if (env_params_.use_external_render == 1) {
    transform = cam_to_world_ ;
  }
  else {
    // Rotate camera frame by 90 to optical frame because we are creating point cloud from images
    Eigen::Isometry3d cam_to_body;
    cam_to_body.matrix() << 0, 0, 1, 0,
                      -1, 0, 0, 0,
                      0, -1, 0, 0,
                      0, 0, 0, 1;
    transform = cam_to_world_ * cam_to_body;
  }
  for (int u = 0; u < s.width; u++) {
    for (int v = 0; v < s.height; v++) {
      vector<int> indices;
      vector<float> sqr_dists;
      pcl::PointXYZRGB point;

      // https://stackoverflow.com/questions/8932893/accessing-certain-pixel-rgb-value-in-opencv
      uint32_t rgbc = ((uint32_t)color_image.at<cv::Vec3b>(v,u)[2] << 16 | (uint32_t)color_image.at<cv::Vec3b>(v,u)[1]<< 8 | (uint32_t)color_image.at<cv::Vec3b>(v,u)[0]);
      point.rgb = *reinterpret_cast<float*>(&rgbc);
      // 255.0 * 10.0
      Eigen::Vector3f point_eig;
      // When using FAT dataset with model
      getGlobalPointCV(u, v,
                            static_cast<float>(depth_image.at<int32_t>(v,u)/depth_factor), transform,
                              point_eig);

      point.x = point_eig[0];
      point.y = point_eig[1];
      point.z = point_eig[2];
      // if (static_cast<float>(depth_image.at<int32_t>(v,u)/depth_factor) > 0)
      // printf("depth data : %f\n", static_cast<float>(depth_image.at<int32_t>(v,u)/depth_factor));

      cloud->points.push_back(point);
    }
  }
  cloud->width = 1;
  cloud->height = cloud->points.size();
  cloud->is_dense = false;
  return cloud;
}

PointCloudPtr EnvObjectRecognition::GetGravityAlignedPointCloud(
  const vector<unsigned short> &depth_image, uint8_t rgb[3]) {

  PointCloudPtr cloud(new PointCloud);

  for (int ii = 0; ii < kNumPixels; ++ii) {
    // Skip if empty pixel
    if (depth_image[ii] == kKinectMaxDepth) {
      continue;
    }

    vector<int> indices;
    vector<float> sqr_dists;
    pcl::PointXYZRGB point;

    uint32_t rgbc = ((uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | (uint32_t)rgb[2]);
    point.rgb = *reinterpret_cast<float*>(&rgbc);

    // int u = kCameraWidth - ii % kCameraWidth;
    int u = ii % kCameraWidth;
    int v = ii / kCameraWidth;
    // int idx = y * camera_width_ + x;
    //         int i_in = (camera_height_ - 1 - y) * camera_width_ + x;
    // point = observed_organized_cloud_->at(v, u);

    Eigen::Vector3f point_eig;
    if (env_params_.use_external_render == 1)
    {
        // Transforms are different when reading from file
        kinect_simulator_->rl_->getGlobalPointCV(u, v,
                                               static_cast<float>(depth_image[ii]) / 1000.0, cam_to_world_,
                                               point_eig);
    }
    else
    {
      v = kCameraHeight - 1 - v;
      kinect_simulator_->rl_->getGlobalPoint(u, v,
                                             static_cast<float>(depth_image[ii]) / 1000.0, cam_to_world_,
                                             point_eig);
    }
    point.x = point_eig[0];
    point.y = point_eig[1];
    point.z = point_eig[2];

    cloud->points.push_back(point);
  }

  cloud->width = 1;
  cloud->height = cloud->points.size();
  cloud->is_dense = false;
  return cloud;
}

PointCloudPtr EnvObjectRecognition::GetGravityAlignedPointCloud(
  const vector<unsigned short> &depth_image,
  const vector<vector<unsigned char>> &color_image) {

    // printf("kCameraWidth : %d\n", kCameraWidth);

    using milli = std::chrono::milliseconds;
    auto start = std::chrono::high_resolution_clock::now();
    if (cost_debug_msgs)
      printf("GetGravityAlignedPointCloud with depth and color\n");
    PointCloudPtr cloud(new PointCloud);
    // TODO
    for (int ii = 0; ii < kNumPixels; ++ii) {
      // Skip if empty pixel
      if (depth_image[ii] >= kKinectMaxDepth) {
        continue;
      }

      vector<int> indices;
      vector<float> sqr_dists;
      pcl::PointXYZRGB point;



      // int u = kCameraWidth - ii % kCameraWidth;
      int u = ii % kCameraWidth;
      int v = ii / kCameraWidth;
      // int idx = y * camera_width_ + x;
      //         int i_in = (camera_height_ - 1 - y) * camera_width_ + x;
      // point = observed_organized_cloud_->at(v, u);

      Eigen::Vector3f point_eig;
      if (env_params_.use_external_render == 1)
      {
          uint32_t rgbc = ((uint32_t)color_image[ii][0] << 16
            | (uint32_t)color_image[ii][1] << 8
            | (uint32_t)color_image[ii][2]
          );
          point.rgb = *reinterpret_cast<float*>(&rgbc);
          // Transforms are different when reading from file
          kinect_simulator_->rl_->getGlobalPointCV(u, v,
                                                static_cast<float>(depth_image[ii]) / 1000.0, cam_to_world_,
                                                point_eig);
      }
      else
      {
        v = kCameraHeight - 1 - v;
        if (perch_params_.use_color_cost)
        {
          uint32_t rgbc = ((uint32_t)color_image[ii][0] << 16
              | (uint32_t)color_image[ii][1] << 8
              | (uint32_t)color_image[ii][2]
          );
          // cout << "color : " << rgbc << endl;
          point.rgb = *reinterpret_cast<float*>(&rgbc);
        }
        kinect_simulator_->rl_->getGlobalPoint(u, v,
                                              static_cast<float>(depth_image[ii]) / 1000.0, cam_to_world_,
                                              point_eig);
      }
      point.x = point_eig[0];
      point.y = point_eig[1];
      point.z = point_eig[2];

      cloud->points.push_back(point);
    }
    cloud->width = 1;
    cloud->height = cloud->points.size();
    cloud->is_dense = false;

    auto finish = std::chrono::high_resolution_clock::now();
    if (cost_debug_msgs)
      std::cout << "GetGravityAlignedPointCloud() took "
                << std::chrono::duration_cast<milli>(finish - start).count()
                << " milliseconds\n";
    if (perch_params_.use_downsampling) {
      cloud = DownsamplePointCloud(cloud, perch_params_.downsampling_leaf_size);
    }
    if (cost_debug_msgs)
      printf("GetGravityAlignedPointCloud with depth and color, cloud size : %d \n", cloud->points.size());

    return cloud;
}

PointCloudPtr EnvObjectRecognition::GetGravityAlignedPointCloud(
  const vector<unsigned short> &depth_image) {
    PointCloudPtr cloud(new PointCloud);
    uint8_t rgb[3] = {255,0,0};
    cloud = GetGravityAlignedPointCloud(depth_image, rgb);
    return cloud;
}

PointCloudPtr EnvObjectRecognition::GetGravityAlignedOrganizedPointCloud(
  const vector<unsigned short> &depth_image) {
  PointCloudPtr cloud(new PointCloud);
  cloud->width = kCameraWidth;
  cloud->height = kCameraHeight;
  cloud->points.resize(kNumPixels);
  cloud->is_dense = true;

  for (int ii = 0; ii < kNumPixels; ++ii) {
    auto &point = cloud->points[VectorIndexToPCLIndex(ii)];

    // Skip if empty pixel
    if (depth_image[ii] == kKinectMaxDepth) {
      point.x = NAN;
      point.y = NAN;
      point.z = NAN;
      continue;
    }

    // int u = kCameraWidth - ii % kCameraWidth;
    int u = ii % kCameraWidth;
    int v = ii / kCameraWidth;
    v = kCameraHeight - 1 - v;
    // int idx = y * camera_width_ + x;
    //         int i_in = (camera_height_ - 1 - y) * camera_width_ + x;
    // point = observed_organized_cloud_->at(v, u);

    Eigen::Vector3f point_eig;
    kinect_simulator_->rl_->getGlobalPoint(u, v,
                                           static_cast<float>(depth_image[ii]) / 1000.0, cam_to_world_,
                                           point_eig);
    point.x = point_eig[0];
    point.y = point_eig[1];
    point.z = point_eig[2];
  }

  return cloud;
}

vector<unsigned short> EnvObjectRecognition::GetDepthImageFromPointCloud(
  const PointCloudPtr &cloud) {
  vector<unsigned short> depth_image(kNumPixels, kKinectMaxDepth);

  for (size_t ii = 0; ii < cloud->size(); ++ii) {
    PointT point = cloud->points[ii];
    Eigen::Vector3f world_point(point.x, point.y, point.z);
    int u = 0;
    int v = 0;
    float range = 0.0;
    kinect_simulator_->rl_->getCameraCoordinate(cam_to_world_, world_point, u, v,
                                                range);
    v = kCameraHeight - 1 - v;

    if (v < 0 || u < 0 || v >= kCameraHeight || u >= kCameraWidth) {
      continue;
    }

    const int idx = v * kCameraWidth + u;
    assert(idx >= 0 && idx < kNumPixels);
    depth_image[idx] = std::min(static_cast<unsigned short>(1000.0 * range),
                                depth_image[idx]);
  }

  return depth_image;
}

void EnvObjectRecognition::PrintValidStates() {
  GraphState source_state;
  pcl::PCDWriter writer;

  for (int ii = 0; ii < env_params_.num_models; ++ii) {
    PointCloudPtr cloud(new PointCloud);

    for (double x = env_params_.x_min; x <= env_params_.x_max;
         x += env_params_.res) {
      for (double y = env_params_.y_min; y <= env_params_.y_max;
           y += env_params_.res) {
        for (double theta = 0; theta < 2 * M_PI; theta += env_params_.theta_res) {
          ContPose p(x, y, 0.0, 0.0, 0.0, theta);

          if (!IsValidPose(source_state, ii, p)) {
            continue;
          }

          PointT point;
          point.x = p.x();
          point.y = p.y();
          point.z = p.z();
          cloud->points.push_back(point);

          // If symmetric object, don't iterate over all thetas
          if (obj_models_[ii].symmetric()) {
            break;
          }
        }
      }
    }

    cloud->width = 1;
    cloud->height = cloud->points.size();
    cloud->is_dense = false;

    if (mpi_comm_->rank() == kMasterRank) {
      std::stringstream ss;
      ss.precision(20);
      ss << debug_dir_ + "valid_cloud_" << ii << ".pcd";
      writer.writeBinary (ss.str(), *cloud);
    }
  }
}

void EnvObjectRecognition::PrintState(int state_id, string fname) {

  GraphState s;

  if (adjusted_states_.find(state_id) != adjusted_states_.end()) {
    s = adjusted_states_[state_id];
  } else {
    s = hash_manager_.GetState(state_id);
  }

  PrintState(s, fname);
  return;
}

void EnvObjectRecognition::PrintState(int state_id, string fname, string cname) {

  GraphState s;

  if (adjusted_states_.find(state_id) != adjusted_states_.end()) {
    s = adjusted_states_[state_id];
  } else {
    s = hash_manager_.GetState(state_id);
  }

  PrintState(s, fname, cname);
  return;
}

void EnvObjectRecognition::PrintState(GraphState s, string fname) {

  printf("Num objects: %zu\n", s.NumObjects());
  std::cout << s << std::endl;

  vector<unsigned short> depth_image;
  GetDepthImage(s, &depth_image);
  PrintImage(fname, depth_image);
  return;
}

void EnvObjectRecognition::PrintState(GraphState s, string fname, string cfname) {
  // Print state with color image always
  printf("Num objects: %zu\n", s.NumObjects());
  std::cout << s << std::endl;
  bool kUseColorCostOriginal = perch_params_.use_color_cost;
  if (!perch_params_.use_color_cost)
  {
    perch_params_.use_color_cost = true;
  }

  vector<unsigned short> depth_image;
  cv::Mat cv_depth_image, cv_color_image;
  vector<vector<unsigned char>> color_image;
  int num_occluders = 0;

  GetDepthImage(s, &depth_image, &color_image,
                cv_depth_image, cv_color_image, &num_occluders, false);
  // cv::imwrite(fname.c_str(), cv_depth_image);
  PrintImage(fname, depth_image);
  cv::imwrite(cfname.c_str(), cv_color_image);
  // PrintImage(fname, depth_image);
  // PrintImage(cfname, color_image);

  perch_params_.use_color_cost = kUseColorCostOriginal;
  return;
}

void EnvObjectRecognition::depthCVToShort(cv::Mat input_image,
                                      vector<unsigned short> *depth_image) {
    printf("depthCVToShort()\n");
    cv::Size s = input_image.size();
    const double range = double(max_observed_depth_ - min_observed_depth_);

    depth_image->resize(s.height * s.width, kKinectMaxDepth);
    // for (int ii = 0; ii < s.height; ++ii) {
    //   for (int jj = 0; jj < s.width; ++jj) {
    //     depth_image->push_back(0);
    //   }
    // }
    for (int ii = 0; ii < s.height; ++ii) {
      for (int jj = 0; jj < s.width; ++jj) {
        int idx = ii * s.width + jj;
        // if (env_params_.use_external_pose_list == 1) {
        //   if (static_cast<unsigned short>(input_image.at<unsigned short>(ii,jj)) < kKinectMaxDepth)
        //     depth_image->at(idx)  =  input_image.at<unsigned short>(ii,jj)/10000 * 1000;
        //   else
        //     depth_image->at(idx)  =  0;
        // }
        // else {
          depth_image->at(idx)  =  static_cast<unsigned short>(input_image.at<uchar>(ii,jj));
        // }
      }
    }
    printf("depthCVToShort() DoneD\n");

}


void EnvObjectRecognition::colorCVToShort(cv::Mat input_image,
                                      vector<vector<unsigned char>> *color_image) {
    printf("colorCVToShort()\n");
    cv::Size s = input_image.size();
    vector<unsigned char> color_vector{'0','0','0'};
    // for (int ii = 0; ii < s.height; ++ii) {
    //   for (int jj = 0; jj < s.width; ++jj) {
    //     color_image->push_back(color_vector);
    //   }
    // }
    color_image->resize(s.height * s.width, color_vector);

    for (int ii = 0; ii < s.height; ++ii) {
      for (int jj = 0; jj < s.width; ++jj) {
        int idx = ii * s.width + jj;
        // std:cout <<
        //   (unsigned short)input_image.at<cv::Vec3b>(ii,jj)[2] << " " <<
        //   (unsigned short)input_image.at<cv::Vec3b>(ii,jj)[1] << " " <<
        //   (unsigned short)input_image.at<cv::Vec3b>(ii,jj)[0] << endl;

        vector<unsigned char> color_vector{
          (unsigned char)input_image.at<cv::Vec3b>(ii,jj)[2],
          (unsigned char)input_image.at<cv::Vec3b>(ii,jj)[1],
          (unsigned char)input_image.at<cv::Vec3b>(ii,jj)[0]
        };

        color_image->at(idx) = color_vector;
      }
    }
    printf("colorCVToShort() Done\n");
}

void EnvObjectRecognition::CVToShort(cv::Mat *input_depth_image,
                                    cv::Mat *input_color_image,
                                    vector<unsigned short> *depth_image,
                                    vector<vector<unsigned char>> *color_image) {
    using milli = std::chrono::milliseconds;
    auto start = std::chrono::high_resolution_clock::now();
    printf("CVToShort()\n");
    assert(input_color_image->size() == input_depth_image->size());

    cv::Size s = input_color_image->size();
    vector<unsigned char> color_vector{'0','0','0'};
    depth_image->resize(s.height * s.width, kKinectMaxDepth);
    color_image->resize(s.height * s.width, color_vector);

    // *depth_image = (unsigned short) input_depth_image->data;
    // std::vector<cv::Point2f> ptvec = cv::Mat_<cv::Point2f>(*input_depth_image);
    // input_depth_image->convertTo(*input_depth_image, CV_32F);
    // depth_image->resize(s.height * s.width, (unsigned short*)input_depth_image->data);
    // depth_image = reinterpret_cast <unsigned short *>(input_depth_image->data);
    // depth_image->assign(input_depth_image->begin<unsigned short>(), input_depth_image->end<unsigned short>());
    // vector<cv::Vec3b> *color_image_vec3b;
    // color_image_vec3b->assign(input_color_image->begin<cv::Vec3b>(), input_color_image->end<cv::Vec3b>());

    // color_image->resize(s.height * s.width, (unsigned short*)input_color_image->data);

    for (int ii = 0; ii < s.height; ++ii) {
      for (int jj = 0; jj < s.width; ++jj) {
        int idx = ii * s.width + jj;
        if (input_depth_image->at<unsigned short>(ii, jj) < kKinectMaxDepth) {
            depth_image->at(idx) = (input_depth_image->at<unsigned short>(ii, jj));
            // uint8_t r = input_color_image->at<cv::Vec3b>(ii,jj)[2];
            // uint8_t g = input_color_image->at<cv::Vec3b>(ii,jj)[1];
            // uint8_t b = input_color_image->at<cv::Vec3b>(ii,jj)[0];
            // uint32_t rgb = ((uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b);
            // uint32_t rgb = ((uint32_t)input_color_image->at<cv::Vec3b>(ii,jj)[2] << 16
            // | (uint32_t)input_color_image->at<cv::Vec3b>(ii,jj)[1]<< 8
            // | (uint32_t)input_color_image->at<cv::Vec3b>(ii,jj)[0]);

            // // int32_t rgb = (r << 16) | (g << 8) | b;

            // float color = *reinterpret_cast<float*>(&rgb);
            // if (color > 0.00001)
            //   printf("%d\n", input_color_image->at<cv::Vec3b>(ii,jj)[2]);

            vector<unsigned char> color_vector{
              input_color_image->at<cv::Vec3b>(ii,jj)[2],
              input_color_image->at<cv::Vec3b>(ii,jj)[1],
              input_color_image->at<cv::Vec3b>(ii,jj)[0]
            };
            color_image->at(idx) = color_vector;

            // color_image->at(idx) = *reinterpret_cast<float*>(&rgb);
        }
      }
    }
    printf("CVToShort() Done\n");
    auto finish = std::chrono::high_resolution_clock::now();
    std::cout << "CVToShort() took "
              << std::chrono::duration_cast<milli>(finish - start).count()
              << " milliseconds\n";
}

void EnvObjectRecognition::PrintImage(string fname,
                                      const vector<unsigned short> &depth_image,
                                      bool show_image_window) {

  assert(depth_image.size() != 0);
  static cv::Mat image;
  image.create(kCameraHeight, kCameraWidth, CV_8UC1);

  const double range = double(max_observed_depth_ - min_observed_depth_);

  for (int ii = 0; ii < kCameraHeight; ++ii) {
    for (int jj = 0; jj < kCameraWidth; ++jj) {
      int idx = ii * kCameraWidth + jj;
      if (depth_image[idx] >= kKinectMaxDepth) {
        image.at<uchar>(ii, jj) = 0;
      } else {
        image.at<uchar>(ii, jj) = 255;
      }
      // printf("%d\n", depth_image[idx]);
      // if (depth_image[idx] > max_observed_depth_ ||
      //     depth_image[idx] == kKinectMaxDepth) {
      //   image.at<uchar>(ii, jj) = 0;
      // }
      // else if (depth_image[idx] < min_observed_depth_) {
      //   image.at<uchar>(ii, jj) = 255;
      // }
      // else {
      //   image.at<uchar>(ii, jj) = static_cast<uchar>(255.0 - double(
      //                                                  depth_image[idx] - min_observed_depth_) * 255.0 / range);
      // }
    }
  }

  static cv::Mat c_image;
  cv::applyColorMap(image, c_image, cv::COLORMAP_JET);

  // Convert background to white to make pretty.
  for (int ii = 0; ii < kCameraHeight; ++ii) {
    for (int jj = 0; jj < kCameraWidth; ++jj) {
      if (image.at<uchar>(ii, jj) == 0) {
        c_image.at<cv::Vec3b>(ii, jj)[0] = 0;
        c_image.at<cv::Vec3b>(ii, jj)[1] = 0;
        c_image.at<cv::Vec3b>(ii, jj)[2] = 0;
      }
    }
  }

  cv::imwrite(fname.c_str(), c_image);

  if (show_image_window) {
    // if (fname.find("expansion") != string::npos) {
      cv::imshow(fname, image);
      cv::waitKey(10);
    // }
  }

  //http://docs.opencv.org/modules/contrib/doc/facerec/colormaps.html
}

void EnvObjectRecognition::PrintImage(string fname,
                                      const vector<unsigned short> &depth_image) {

  assert(depth_image.size() != 0);
  static cv::Mat image;
  image.create(kCameraHeight, kCameraWidth, CV_8UC1);

  const double range = double(max_observed_depth_ - min_observed_depth_);

  for (int ii = 0; ii < kCameraHeight; ++ii) {
    for (int jj = 0; jj < kCameraWidth; ++jj) {
      int idx = ii * kCameraWidth + jj;
      if (depth_image[idx] >= kKinectMaxDepth) {
        image.at<uchar>(ii, jj) = 0;
      } else {
        image.at<uchar>(ii, jj) = 255;
      }
      // printf("%d\n", depth_image[idx]);
      // if (depth_image[idx] > max_observed_depth_ ||
      //     depth_image[idx] == kKinectMaxDepth) {
      //   image.at<uchar>(ii, jj) = 0;
      // }
      // else if (depth_image[idx] < min_observed_depth_) {
      //   image.at<uchar>(ii, jj) = 255;
      // }
      // else {
      //   image.at<uchar>(ii, jj) = static_cast<uchar>(255.0 - double(
      //                                                  depth_image[idx] - min_observed_depth_) * 255.0 / range);
      // }
    }
  }

  static cv::Mat c_image;
  cv::applyColorMap(image, c_image, cv::COLORMAP_JET);

  // Convert background to white to make pretty.
  for (int ii = 0; ii < kCameraHeight; ++ii) {
    for (int jj = 0; jj < kCameraWidth; ++jj) {
      if (image.at<uchar>(ii, jj) == 0) {
        c_image.at<cv::Vec3b>(ii, jj)[0] = 0;
        c_image.at<cv::Vec3b>(ii, jj)[1] = 0;
        c_image.at<cv::Vec3b>(ii, jj)[2] = 0;
      }
    }
  }

  cv::imwrite(fname.c_str(), c_image);

  if (perch_params_.vis_expanded_states) {
    if (fname.find("expansion") != string::npos) {
      cv::imshow("expansions", image);
      cv::waitKey(10);
    }
  }

  //http://docs.opencv.org/modules/contrib/doc/facerec/colormaps.html
}

bool EnvObjectRecognition::IsGoalState(GraphState state) {
  if (static_cast<int>(state.NumObjects()) ==  env_params_.num_objects) {
    return true;
  }

  return false;
}


const float *EnvObjectRecognition::GetDepthImage(GraphState s,
                                                 vector<unsigned short> *depth_image) {

  int num_occluders = 0;
  vector<vector<unsigned char>> color_image;
  cv::Mat cv_depth_image, cv_color_image;
  return GetDepthImage(s, depth_image, &color_image, cv_depth_image, cv_color_image, &num_occluders, false);
}
//GetDepthImage after append a new object
const float *EnvObjectRecognition::GetDepthImage(GraphState s,
                                                 vector<unsigned short> *depth_image,
                                                 vector<vector<unsigned char>> *color_image,
                                                 cv::Mat *cv_depth_image,
                                                 cv::Mat *cv_color_image) {

  int num_occluders = 0;
  if (env_params_.use_external_render == 1) {
      return GetDepthImage(s, depth_image, color_image, cv_depth_image, cv_color_image, &num_occluders);
  } else {
      return GetDepthImage(s, depth_image, color_image, *cv_depth_image, *cv_color_image, &num_occluders, false);
  }
}

cv::Mat rotate(cv::Mat src, double angle)
{
    cv::Mat dst;
    cv::Point2f pt(src.cols/2., src.rows/2.);
    cv::Mat r = cv::getRotationMatrix2D(pt, angle, 1.0);
    cv::warpAffine(src, dst, r, cv::Size(src.cols, src.rows));
    return dst;
}

const float *EnvObjectRecognition::GetDepthImage(GraphState &s,
                                                std::vector<unsigned short> *depth_image,
                                                std::vector<std::vector<unsigned char>> *color_image,
                                                cv::Mat &cv_depth_image,
                                                cv::Mat &cv_color_image,
                                                int* num_occluders_in_input_cloud,
                                                bool shift_centroid) {

  using milli = std::chrono::milliseconds;
  auto start = std::chrono::high_resolution_clock::now();

  *num_occluders_in_input_cloud = 0;
  if (scene_ == NULL) {
    printf("ERROR: Scene is not set\n");
  }


  scene_->clear();

  auto &object_states = s.mutable_object_states();
  if (cost_debug_msgs)
    printf("GetDepthImage() for number of objects : %d\n", object_states.size());

  // cout << "External Render :" << env_params_.use_external_render;
   for (size_t ii = 0; ii < object_states.size(); ++ii) {
      const auto &object_state = object_states[ii];
      ObjectModel obj_model = obj_models_[object_state.id()];
      ContPose p = object_state.cont_pose();
      // std::cout << "Object model in pose : " << p << endl;

      // std::cout << "Object model in pose after shift: " << p << endl;
      pcl::PolygonMeshPtr transformed_mesh;
      if (shift_centroid && ii == object_states.size()-1)
      {
        transformed_mesh = obj_model.GetTransformedMeshWithShift(p);
        object_states[ii] = ObjectState(object_state.id(), object_state.symmetric(), p);
      }
      else
      {
        transformed_mesh = obj_model.GetTransformedMesh(p);
      }
      // std::string name;
      // name = "/media/jessy/Data/dataset/saved_ply/";
      // name.append(std::to_string(p.x()));
      // name.append(",");
      // name.append(std::to_string(p.y()));
      // name.append(",");
      // name.append(std::to_string(p.z()));
      // name.append(",");
      // name.append(std::to_string(p.yaw()));
      // name.append(".ply");
      // // pcl::io::savePLYFile(name, *transformed_mesh);
      
      // pcl::io::loadPolygonFilePLY(name,*transformed_mesh);
      
      //pcl::io::loadPCDFile(name,transformed_mesh->cloud);
      PolygonMeshModel::Ptr model = PolygonMeshModel::Ptr (new PolygonMeshModel (
                                                             GL_POLYGON, transformed_mesh));
      scene_->add (model);
      
      
    }
    
    kinect_simulator_->doSim(env_params_.camera_pose);
    const float *depth_buffer = kinect_simulator_->rl_->getDepthBuffer();
    kinect_simulator_->get_depth_image_uint(depth_buffer, depth_image);
    // kinect_simulator_->write_depth_image_uint(depth_buffer, "test_depth.png");

    if (perch_params_.use_color_cost) 
    {
      const uint8_t *color_buffer = kinect_simulator_->rl_->getColorBuffer();
      kinect_simulator_->get_rgb_image_uchar(color_buffer, color_image);
      kinect_simulator_->get_rgb_image_cv(color_buffer, cv_color_image);
      cv::cvtColor(cv_color_image, cv_color_image, CV_BGR2RGB);
      // kinect_simulator_->write_rgb_image(color_buffer, "test_color.png");
    }
    // printf("depth vector max size :%d\n", (int) depth_image->max_size());
    // printf("color vector max size :%d\n", (int) color_image->max_size());
    // cv::Mat cv_image;
    kinect_simulator_->get_depth_image_cv(depth_buffer, cv_depth_image);
    // *cv_depth_image = cv_image;
    // cv_depth_image = cv::Mat(kCameraHeight, kCameraWidth, CV_16UC1, depth_image->data());
    // if (mpi_comm_->rank() == kMasterRank) {
    //   static cv::Mat c_image;
    //   ColorizeDepthImage(cv_depth_image, c_image, min_observed_depth_, max_observed_depth_);
    //   cv::imshow("depth image", c_image);
    //   cv::waitKey(1);
    // }

    // Consider occlusions from non-modeled objects.
    if (perch_params_.use_clutter_mode) {
      for (size_t ii = 0; ii < depth_image->size(); ++ii) {
        if (observed_depth_image_[ii] < (depth_image->at(ii) - kOcclusionThreshold) &&
            depth_image->at(ii) != kKinectMaxDepth) {
          depth_image->at(ii) = kKinectMaxDepth;
          (*num_occluders_in_input_cloud)++;
        }
      }
    }

    auto finish = std::chrono::high_resolution_clock::now();
    std::cout << "GetDepthImage() took "
              << std::chrono::duration_cast<milli>(finish - start).count()
              << " milliseconds\n";
    return depth_buffer;

};
//GetDepthImage with cv:mat color image 
const float *EnvObjectRecognition::GetDepthImage(GraphState s,
                             std::vector<unsigned short> *depth_image,
                             vector<vector<unsigned char>> *color_image,
                             cv::Mat *cv_depth_image,
                             cv::Mat *cv_color_image,
                             int* num_occluders_in_input_cloud) {

  *num_occluders_in_input_cloud = 0;
  if (scene_ == NULL) {
    printf("ERROR: Scene is not set\n");
  }

  scene_->clear();

  const auto &object_states = s.object_states();
  printf("GetDepthImage() color and depth for number of objects : %d\n", object_states.size());
  // const float *depth_buffer;
  kinect_simulator_->doSim(env_params_.camera_pose);
  const float *depth_buffer = kinect_simulator_->rl_->getDepthBuffer();
  if (object_states.size() > 0)
  {
      // printf("Using new render as\n");
      // cv::Mat final_image(kCameraHeight, kCameraWidth, CV_8UC3, cv::Scalar(0,0,0));
      for (size_t ii = 0; ii < object_states.size(); ii++) {
          const auto &object_state = object_states[ii];
          ObjectModel obj_model = obj_models_[object_state.id()];
          ContPose p = object_state.cont_pose();
          std::stringstream ss;
          // cout << p.external_render_path();
          ss << p.external_render_path() << "/" << obj_model.name() << "/" << p.external_pose_id() << "-color.png";
          std::string color_image_path = ss.str();
          printf("State : %d, Path for rendered state's image of %s : %s\n", ii, obj_model.name().c_str(), color_image_path.c_str());

          *cv_color_image = cv::imread(color_image_path);
          // if (mpi_comm_->rank() == kMasterRank) {
            // static cv::Mat c_image;
            // ColorizeDepthImage(cv_depth_image, c_image, min_observed_depth_, max_observed_depth_);


          ss.str("");
          ss << p.external_render_path() << "/" << obj_model.name() << "/" << p.external_pose_id() << "-depth.png";
          color_image_path = ss.str();
          printf("State : %d, Path for rendered state's image of %s : %s\n", ii, obj_model.name().c_str(), color_image_path.c_str());

          *cv_depth_image = cv::imread(color_image_path, CV_LOAD_IMAGE_ANYDEPTH);
          // static cv::Mat c_image;
          // cvToShort(cv_depth_image, depth_image);

          // for (int u = 0; u < kCameraWidth; u++) {
          //   for (int v = 0; v < kCameraHeight; v++) {
          //     // if (final_image.at<unsigned short>(v,u) > 0 && cv_depth_image.at<unsigned short>(v,u) > 0)
          //     {
          //         final_image.at<unsigned short>(v,u) = std::max(final_image.at<unsigned short>(v,u),
          //                                                 cv_depth_image.at<unsigned short>(v,u));
          //     }
          //   }
          // }
          // ColorizeDepthImage(cv_depth_image, c_image, min_observed_depth_, max_observed_depth_);
          // cv::imshow("depth image", c_image);
          // cv::waitKey(1);
          // depthCVToShort(cv_depth_image, depth_image);
          // colorCVToShort(cv_color_image, color_image);
          CVToShort(cv_depth_image, cv_color_image, depth_image, color_image);

          cv_depth_image->release();
          cv_color_image->release();
          // if (object_states.size() > 1)
          // {
          //     // ColorizeDepthImage(cv_depth_image, c_image, min_observed_depth_, max_observed_depth_);
          //     // cv::imshow("depth image", c_image);
          //     // ColorizeDepthImage(final_image, c_image, min_observed_depth_, max_observed_depth_);
          //     cv::imshow("comopsed depth image", final_image);
          //     cv::waitKey(1);
          //     auto gravity_aligned_point_cloud = GetGravityAlignedPointCloudCV(final_image, cv_color_image);
          //
          //     PrintPointCloud(gravity_aligned_point_cloud, 1);
          // }

      }
      // if (object_states.size() > 1)
      // {
      //     // ColorizeDepthImage(cv_depth_image, c_image, min_observed_depth_, max_observed_depth_);
      //     // cv::imshow("depth image", c_image);
      //     // ColorizeDepthImage(final_image, c_image, min_observed_depth_, max_observed_depth_);
      //     cv::imshow("comopsed depth image", final_image);
      //     cv::waitKey(1);
      //     auto gravity_aligned_point_cloud = GetGravityAlignedPointCloudCV(final_image, final_image);

      //     PrintPointCloud(gravity_aligned_point_cloud, 1);
      // }
  }
  else
  {
    printf("Using original render as\n");
    // TODO change this to just creating a blank image
    for (size_t ii = 0; ii < object_states.size(); ++ii) {
      const auto &object_state = object_states[ii];
      ObjectModel obj_model = obj_models_[object_state.id()];
      ContPose p = object_state.cont_pose();
      // std::cout << "Object model in pose : " << p << endl;
      auto transformed_mesh = obj_model.GetTransformedMesh(p);

      PolygonMeshModel::Ptr model = PolygonMeshModel::Ptr (new PolygonMeshModel (
                                                             GL_POLYGON, transformed_mesh));
      scene_->add (model);
    }

    // kinect_simulator_->doSim(env_params_.camera_pose);
    // const float *depth_buffer = kinect_simulator_->rl_->getDepthBuffer();
    kinect_simulator_->get_depth_image_uint(depth_buffer, depth_image);

    // Init blank
    vector<unsigned char> color_vector{'0','0','0'};
    color_image->clear();
    color_image->resize(depth_image->size(), color_vector);
  }
  //
  // kinect_simulator_->get_depth_image_cv(depth_buffer, depth_image);
  // cv_depth_image = cv::Mat(kCameraHeight, kCameraWidth, CV_16UC1, depth_image->data());
  // if (mpi_comm_->rank() == kMasterRank) {
  //   static cv::Mat c_image;
  //   ColorizeDepthImage(cv_depth_image, c_image, min_observed_depth_, max_observed_depth_);
  //   cv::imshow("depth image", c_image);
  //   cv::waitKey(1);
  // }

  // Consider occlusions from non-modeled objects.
  if (perch_params_.use_clutter_mode) {
    for (size_t ii = 0; ii < depth_image->size(); ++ii) {
      if (observed_depth_image_[ii] < (depth_image->at(ii) - kOcclusionThreshold) &&
          depth_image->at(ii) != kKinectMaxDepth) {
        depth_image->at(ii) = kKinectMaxDepth;
        (*num_occluders_in_input_cloud)++;
      }
    }
  }

  return depth_buffer;

};

void EnvObjectRecognition::SetCameraPose(Eigen::Isometry3d camera_pose) {
  env_params_.camera_pose = camera_pose;
  cam_to_world_ = camera_pose;
  // cam_to_world_.matrix() << -0.000109327,    -0.496186,     0.868216,     0.436204,
  //                     -1,  5.42467e-05, -9.49191e-05,    0.0324911,
  //            -4.0826e-10,    -0.868216,    -0.496186,     0.573853,
  //                      0,            0,            0,            1;
  return;
}

void EnvObjectRecognition::SetTableHeight(double height) {
  env_params_.table_height = height;
}

double EnvObjectRecognition::GetTableHeight() {
  return env_params_.table_height;
}

void EnvObjectRecognition::SetBounds(double x_min, double x_max, double y_min,
                                     double y_max) {
  env_params_.x_min = x_min;
  env_params_.x_max = x_max;
  env_params_.y_min = y_min;
  env_params_.y_max = y_max;
}

void EnvObjectRecognition::SetObservation(int num_objects,
                                          const vector<unsigned short> observed_depth_image) {
  observed_depth_image_.clear();
  observed_depth_image_ = observed_depth_image;
  env_params_.num_objects = num_objects;

  // Compute the range in observed_color image
  unsigned short observed_min_depth = kKinectMaxDepth;
  unsigned short observed_max_depth = 0;

  if (observed_depth_image_.size() > 0)
  {
    for (int ii = 0; ii < kNumPixels; ++ii) {
      if (observed_depth_image_[ii] < observed_min_depth) {
        observed_min_depth = observed_depth_image_[ii];
      }

      if (observed_depth_image_[ii] != kKinectMaxDepth &&
          observed_depth_image_[ii] > observed_max_depth) {
        observed_max_depth = observed_depth_image_[ii];
      }
    }
  }

  // NOTE: if we don't have an original_point_cloud (which will always exist
  // when using dealing with real world data), then we will generate a point
  // cloud using the depth image.
  if (!original_input_cloud_->empty()) {
    printf("Observed Point cloud from original_input_cloud_ of size : %d\n", original_input_cloud_->points.size());
    *observed_cloud_ = *original_input_cloud_;
  } else {
    printf("Observed Point cloud from observed_depth_image_\n");
    PointCloudPtr gravity_aligned_point_cloud(new PointCloud);
    gravity_aligned_point_cloud = GetGravityAlignedPointCloud(
                                    observed_depth_image_);

    if (mpi_comm_->rank() == kMasterRank && perch_params_.print_expanded_states) {
      std::stringstream ss;
      ss.precision(20);
      ss << debug_dir_ + "gravity_aligned_cloud.pcd";
      pcl::PCDWriter writer;
      writer.writeBinary (ss.str()  , *gravity_aligned_point_cloud);
    }

    *observed_cloud_  = *gravity_aligned_point_cloud;
  }

  printf("Use Dowsampling: %d\n", perch_params_.use_downsampling);
  if (perch_params_.use_downsampling) {
    observed_cloud_ = DownsamplePointCloud(observed_cloud_, perch_params_.downsampling_leaf_size);
  }
  for (int i = 0; i < segmented_object_clouds.size(); i++)
  {
    if (perch_params_.use_downsampling) {
      segmented_object_clouds[i] = DownsamplePointCloud(segmented_object_clouds[i], perch_params_.downsampling_leaf_size);
      printf("Setting downsampled segmented cloud of size : %d\n", segmented_object_clouds[i]->points.size());
    }
    // PrintPointCloud(segmented_object_clouds[i], 1, render_point_cloud_topic);
    // std::this_thread::sleep_for(std::chrono::milliseconds(500));
    pcl::search::KdTree<PointT>::Ptr object_knn;
    object_knn.reset(new pcl::search::KdTree<PointT>(true));
    object_knn->setInputCloud(segmented_object_clouds[i]);
    segmented_object_knn.push_back(object_knn);
  }

  // Remove outlier points - possible in 6D due to bad segmentation
  if (env_params_.use_external_pose_list == 1)
  {
    pcl::StatisticalOutlierRemoval<pcl::PointXYZRGB> sor;
    sor.setInputCloud (observed_cloud_);
    sor.setMeanK (50);
    sor.setStddevMulThresh (1.0);
    sor.filter (*observed_cloud_);
  }

  vector<int> nan_indices;
  downsampled_observed_cloud_ = DownsamplePointCloud(observed_cloud_, 0.01);
  if (IsMaster(mpi_comm_)) {
    for (int i = 0; i < 10; i++)
      PrintPointCloud(downsampled_observed_cloud_, 1, downsampled_input_point_cloud_topic);
  }

  knn.reset(new pcl::search::KdTree<PointT>(true));
  printf("Setting knn with cloud of size : %d\n", observed_cloud_->points.size());
  // if (IsMaster(mpi_comm_)) {
  //     PrintPointCloud(observed_cloud_, 1, render_point_cloud_topic);
  // }
  knn->setInputCloud(observed_cloud_);

  // Aditya commented
  // if (mpi_comm_->rank() == kMasterRank) {
  //   if (env_params_.use_external_pose_list == 0)
  //     LabelEuclideanClusters();
  // }

  // Project point cloud to table.
  *projected_cloud_ = *observed_cloud_;

  // Aditya
  valid_indices_.clear();
  valid_indices_.reserve(projected_cloud_->size());

  for (size_t ii = 0; ii < projected_cloud_->size(); ++ii) {
    if (!(std::isnan(projected_cloud_->points[ii].z) ||
          std::isinf(projected_cloud_->points[ii].z))) {
      valid_indices_.push_back(static_cast<int>(ii));
    }

    projected_cloud_->points[ii].z = env_params_.table_height;
  }

  // Project the constraint cloud as well.
  *projected_constraint_cloud_ = *constraint_cloud_;

  for (size_t ii = 0; ii < projected_constraint_cloud_->size(); ++ii) {
    projected_constraint_cloud_->points[ii].z = env_params_.table_height;
  }

  printf("Setting projected_knn_ with cloud of size : %d\n", projected_cloud_->points.size());
  projected_knn_.reset(new pcl::search::KdTree<PointT>(true));
  projected_knn_->setInputCloud(projected_cloud_);

  // Project the downsampled observed_color cloud
  *downsampled_projected_cloud_ = *downsampled_observed_cloud_;
  // downsampled_projected_cloud_ = DownsamplePointCloud(projected_cloud_, 0.005);
  for (size_t ii = 0; ii < downsampled_projected_cloud_->size(); ++ii) {
    downsampled_projected_cloud_->points[ii].z = env_params_.table_height;
  }
  printf("Setting downsampled_projected_knn_ with cloud of size : %d\n", downsampled_projected_cloud_->points.size());
  downsampled_projected_knn_.reset(new pcl::search::KdTree<PointT>(true));
  downsampled_projected_knn_->setInputCloud(downsampled_projected_cloud_);

  min_observed_depth_ = kKinectMaxDepth;
  max_observed_depth_ = 0;

  if (observed_depth_image_.size() > 0)
  {
    for (int ii = 0; ii < kCameraHeight; ++ii) {
      for (int jj = 0; jj < kCameraWidth; ++jj) {
        int idx = ii * kCameraWidth + jj;

        if (observed_depth_image_[idx] == kKinectMaxDepth) {
          continue;
        }

        if (max_observed_depth_ < observed_depth_image_[idx]) {
          max_observed_depth_ = observed_depth_image_[idx];
        }

        if (min_observed_depth_ > observed_depth_image_[idx]) {
          min_observed_depth_ = observed_depth_image_[idx];
        }
      }
    }

    if (mpi_comm_->rank() == kMasterRank) {
      PrintImage(debug_dir_ + string("input_depth_image.png"),
                observed_depth_image_);
    }
  }

  // if (mpi_comm_->rank() == kMasterRank && perch_params_.print_expanded_states) {
  //   std::stringstream ss;
  //   ss.precision(20);
  //   ss << debug_dir_ + "obs_cloud" << ".pcd";
  //   pcl::PCDWriter writer;
  //   writer.writeBinary (ss.str()  , *observed_cloud_);
  // }


  // if (mpi_comm_->rank() == kMasterRank && perch_params_.print_expanded_states) {
  //   std::stringstream ss;
  //   ss.precision(20);
  //   ss << debug_dir_ + "projected_cloud" << ".pcd";
  //   pcl::PCDWriter writer;
  //   writer.writeBinary (ss.str()  , *projected_cloud_);
  // }
  printf("SetObservation() done\n");
}

void EnvObjectRecognition::ResetEnvironmentState() {
  if (IsMaster(mpi_comm_)) {
    printf("-------------------Resetting Environment State-------------------\n");
  }

  GraphState start_state, goal_state;

  hash_manager_.Reset();
  adjusted_states_.clear();
  env_stats_.scenes_rendered = 0;
  env_stats_.scenes_valid = 0;

  const ObjectState special_goal_object_state(-1, false, DiscPose(0, 0, 0, 0, 0,
                                                                  0));
  goal_state.mutable_object_states().push_back(
    special_goal_object_state); // This state should never be generated during the search

  env_params_.start_state_id = hash_manager_.GetStateIDForceful(
                                 start_state); // Start state is the empty state
  env_params_.goal_state_id = hash_manager_.GetStateIDForceful(goal_state);

  if (IsMaster(mpi_comm_)) {
    std::cout << "Goal state: " << std::endl << goal_state << std::endl;
    std::cout << "Start state ID: " << env_params_.start_state_id << std::endl;
    std::cout << "Goal state ID: " << env_params_.goal_state_id << std::endl;
    hash_manager_.Print();
  }

  // TODO: group environment state variables into struct.
  minz_map_.clear();
  maxz_map_.clear();
  g_value_map_.clear();
  succ_cache.clear();
  valid_succ_cache.clear();
  cost_cache.clear();
  last_object_rendering_cost_.clear();
  depth_image_cache_.clear();
  counted_pixels_map_.clear();
  adjusted_single_object_depth_image_cache_.clear();
  unadjusted_single_object_depth_image_cache_.clear();
  adjusted_single_object_state_cache_.clear();
  valid_indices_.clear();

  minz_map_[env_params_.start_state_id] = 0;
  maxz_map_[env_params_.start_state_id] = 0;
  g_value_map_[env_params_.start_state_id] = 0;

  observed_cloud_.reset(new PointCloud);
  original_input_cloud_.reset(new PointCloud);
  projected_cloud_.reset(new PointCloud);
  observed_organized_cloud_.reset(new PointCloud);
  downsampled_observed_cloud_.reset(new PointCloud);
  downsampled_projected_cloud_.reset(new PointCloud);
}

void EnvObjectRecognition::SetObservation(vector<int> object_ids,
                                          vector<ContPose> object_poses) {
  assert(object_ids.size() == object_poses.size());
  GraphState s;

  ResetEnvironmentState();

  for (size_t ii = 0; ii < object_ids.size(); ++ii) {
    if (object_ids[ii] >= env_params_.num_models) {
      printf("ERROR: Invalid object ID %d when setting ground truth\n",
             object_ids[ii]);
    }

    s.AppendObject(ObjectState(object_ids[ii],
                               obj_models_[object_ids[ii]].symmetric(), object_poses[ii]));
  }


  vector<unsigned short> depth_image;
  GetDepthImage(s, &depth_image);
  kinect_simulator_->rl_->getOrganizedPointCloud(observed_organized_cloud_, true,
                                                 env_params_.camera_pose);
  // Precompute RCNN heuristics.
  SetObservation(object_ids.size(), depth_image);
}

cv::Mat equalizeIntensity(const cv::Mat& inputImage)
{
    if(inputImage.channels() >= 3)
    {
        cv::Mat ycrcb;
        cv::cvtColor(inputImage,ycrcb,CV_BGR2YCrCb);

        vector<cv::Mat> channels;
        cv::split(ycrcb,channels);

        cv::equalizeHist(channels[0], channels[0]);

        cv::Mat result;
        cv::merge(channels,ycrcb);
        cv::cvtColor(ycrcb,result,CV_YCrCb2BGR);

        return result;
    }

    return cv::Mat();
}


void EnvObjectRecognition::SetStaticInput(const RecognitionInput &input)
{
  /* 
  * Set static input variables from RecognitionInput such as object files etc
  * Useful when running on robot in tracking mode - load models once and keep sending object requests
  */
  ResetEnvironmentState();

  *constraint_cloud_ = input.constraint_cloud;
  env_params_.use_external_render = input.use_external_render;
  env_params_.reference_frame_ = input.reference_frame_;
  env_params_.use_external_pose_list = input.use_external_pose_list;
  env_params_.use_icp = input.use_icp;
  env_params_.shift_pose_centroid = input.shift_pose_centroid;
  env_params_.rendered_root_dir = input.rendered_root_dir; 

  // Loads object files and preprocess them to get circumscribed radius, preprocessing transform etc.
  LoadObjFiles(model_bank_, input.model_names);

  SetBounds(input.x_min, input.x_max, input.y_min, input.y_max);
  SetTableHeight(input.table_height);

  printf("External Render : %d\n", env_params_.use_external_render);
  printf("External Pose List : %d\n", env_params_.use_external_pose_list);
  printf("ICP : %d\n", env_params_.use_icp);
  printf("Shift Pose Centroid : %d\n", env_params_.shift_pose_centroid);
  printf("Rendered Root Dir : %s\n", env_params_.rendered_root_dir.c_str());
  // If #repetitions is not set, we will assume every unique model appears
  // exactly once in the scene.
  // if (input.model_repetitions.empty()) {
  //   input.model_repetitions.resize(input.model_names.size(), 1);
  // }

}


void EnvObjectRecognition::SetInput(const RecognitionInput &input) {
  /* 
   * Set dynamic input variables from RecognitionInput such as images, input clouds etc.
   */
  chrono::time_point<chrono::system_clock> start, end;
  start = chrono::system_clock::now();

  SetCameraPose(input.camera_pose);
  printf("Depth Factor : %f\n", input.depth_factor);
  PointCloudPtr depth_img_cloud(new PointCloud);
  vector<unsigned short> depth_image;
  vector<vector<uint8_t>> input_color_image_vec(3);
  uint8_t* predicted_mask_image_ptr = NULL;
  double* observed_cloud_bounds_ptr = NULL;
  vector<double> bounds;
  Eigen::Matrix4f* camera_transform_ptr = NULL;
  gpu_stride = perch_params_.gpu_stride;

  if (input.use_input_images)
  {
    printf("Using input images instead of cloud\n");
    cv::Mat cv_depth_image, cv_predicted_mask_image;
    input_depth_image_path = input.input_depth_image;
    
    // Input depth images for 3-Dof (NDDS datasets) and 6-Dof need to be read differently (YCB)
    if (env_params_.use_external_pose_list == 1) {

      // For 6-Dof datasets dataset, we have 16bit images
      cv_depth_image = cv::imread(input.input_depth_image, CV_LOAD_IMAGE_ANYDEPTH);
      cv_predicted_mask_image = cv::imread(input.predicted_mask_image, CV_LOAD_IMAGE_UNCHANGED);

      // Convert 2D image to 1D vector for GPU
      predicted_mask_image.assign(
        cv_predicted_mask_image.ptr<uint8_t>(0), 
        cv_predicted_mask_image.ptr<uint8_t>(0) + env_params_.width * env_params_.height
      );
      predicted_mask_image_ptr = predicted_mask_image.data();
     
      // Index of segmented_object_names variable is the label for corresponding model name in the segmentation mask
      segmented_object_names = input.model_names;

      // Make empty point clouds corresponding to segmented object clouds
      for (int i = 0; i < input.model_names.size(); i++)
      {
        PointCloudPtr cloud(new PointCloud);
        segmented_object_clouds.push_back(cloud);
      }

      // Convert 2D image to 1D vector for GPU
      input_depth_image_vec.assign(
        cv_depth_image.ptr<unsigned short>(0), 
        cv_depth_image.ptr<unsigned short>(0) + env_params_.width * env_params_.height
      );
    }
    else {
      // Loads 8-bit depth images (0-255) range
      // Using CV_LOAD_IMAGE_UNCHANGED to use exact conversion from NDDS documentation /255 * 1000 gives actual distance in cm
      cv_depth_image = cv::imread(input.input_depth_image, CV_LOAD_IMAGE_UNCHANGED);

      // Median blurring to remove some depth holes
      medianBlur(cv_depth_image, cv_depth_image, perch_params_.depth_median_blur); //gpu conveyor
      if (perch_params_.use_gpu)
      {
        // Convert 2D image to 1D vector for GPU
        input_depth_image_vec.assign(
          cv_depth_image.ptr<uchar>(0), 
          cv_depth_image.ptr<uchar>(0) + env_params_.width * env_params_.height
        );

        // Create vector containing 3-Dof table bounds
        bounds.push_back(env_params_.x_max);
        bounds.push_back(env_params_.x_min);
        bounds.push_back(env_params_.y_max);
        bounds.push_back(env_params_.y_min); 
        bounds.push_back(env_params_.table_height + 0.5);
        bounds.push_back(env_params_.table_height);
        observed_cloud_bounds_ptr = bounds.data();

        // Create a camera transform pointer for GPU
        Eigen::Isometry3d cam_to_body;
        cam_to_body.matrix() << 0, 0, 1, 0,
                                -1, 0, 0, 0,
                                0, -1, 0, 0,
                                0, 0, 0, 1;
        // Convert camera pose to optical frame because we are reading image directly
        Eigen::Isometry3d transform_iso = cam_to_world_ * cam_to_body;
        Eigen::Matrix4f transform = transform_iso.matrix ().cast<float> ();

        camera_transform_ptr = &transform;
      }
    }

    cv_input_color_image = cv::imread(input.input_color_image, CV_LOAD_IMAGE_COLOR);

    if (perch_params_.use_gpu)
    {
      // Initial r,g,b vectors from color image for GPU
      vector<cv::Mat> color_channels(3);
      cv::split(cv_input_color_image, color_channels);
      input_color_image_vec[2].assign(
        color_channels[0].ptr<uint8_t>(0), 
        color_channels[0].ptr<uint8_t>(0) + env_params_.width * env_params_.height
      );
      input_color_image_vec[1].assign(
        color_channels[1].ptr<uint8_t>(0), 
        color_channels[1].ptr<uint8_t>(0) + env_params_.width * env_params_.height
      );
      input_color_image_vec[0].assign(
        color_channels[2].ptr<uint8_t>(0), 
        color_channels[2].ptr<uint8_t>(0) + env_params_.width * env_params_.height
      );
    }
    // cv_color_image = equalizeIntensity(cv_color_image);
    
    std::stringstream ss1;
    ss1 << debug_dir_ << "input_color_image.png";
    std::string color_image_path = ss1.str();
    cv::imwrite(color_image_path, cv_input_color_image);
    
    if (perch_params_.use_gpu)
    {
      // If using GPU, then read images and convert to clouds on GPU
      // Also apply bounds (3-Dof) or mask (6-Dof)
      input_depth_factor = input.depth_factor;

      observed_depth_data = input_depth_image_vec.data(); //AD
      std::vector<int> random_poses_occluded(1, 0);
      int* cloud_pose_map;
      std::vector<ObjectState> last_object_states;
      vector<ObjectState> modified_last_object_states;

      cuda_renderer::depth2cloud_global(
          input_depth_image_vec, 
          input_color_image_vec, 
          result_observed_cloud_eigen,
          result_observed_cloud, 
          result_observed_cloud_color, 
          observed_dc_index, 
          observed_point_num, 
          cloud_pose_map, 
          result_observed_cloud_label,
          env_params_.width, 
          env_params_.height,
          1, 
          random_poses_occluded, 
          kCameraCX, 
          kCameraCY, 
          kCameraFX, 
          kCameraFY, 
          input.depth_factor, 
          gpu_stride, 
          gpu_point_dim,
          predicted_mask_image, 
          bounds, 
          camera_transform_ptr
      );
      PrintGPUClouds(
        last_object_states, result_observed_cloud, result_observed_cloud_color, 
        observed_depth_data, observed_dc_index, 1, 
        observed_point_num, gpu_stride, random_poses_occluded.data(), "observed",
        modified_last_object_states, false, gpu_input_point_cloud_topic, true);
      
      // Total observed points in each object, used for calculating observed cost
      if (env_params_.use_external_pose_list == 1) 
        segmented_observed_point_count.resize(segmented_object_names.size(), 0.0);
      
      uint8_t rgb[3] = {0,0,0};
      printf("observed_point_num : %d\n", observed_point_num);

      // Copy GPU clouds to CPU PCL point clouds, if ICP needs to be done on CPU
      for (int i = 0; i < observed_point_num; i++)
      {
        // printf("Label for point %d, %d\n", i, result_observed_cloud_label[i]);
        // Ignore points without segmentation label (ideally there shouldnt be any)
        if (result_observed_cloud_label[i] == -1
          && env_params_.use_external_pose_list == 1) {
            printf("ERROR : invalid label points in observed cloud after filter\n");
        }

        int label_mask_i = result_observed_cloud_label[i];
        pcl::PointXYZRGB point;
        point.x = result_observed_cloud[i + 0*observed_point_num];
        point.y = result_observed_cloud[i + 1*observed_point_num];
        point.z = result_observed_cloud[i + 2*observed_point_num];
        rgb[2] = result_observed_cloud_color[i + 0*observed_point_num];
        rgb[1] = result_observed_cloud_color[i + 1*observed_point_num];
        rgb[0] = result_observed_cloud_color[i + 2*observed_point_num];
        uint32_t rgbc = ((uint32_t)rgb[2] << 16 | (uint32_t)rgb[1]<< 8 | (uint32_t)rgb[0]);
        point.rgb = *reinterpret_cast<float*>(&rgbc);

        depth_img_cloud->points.push_back(point);
        if (env_params_.use_external_pose_list == 1) 
        {
          // segmented_object_clouds[label_mask_i-1]->points.push_back(point);
          // segmented_observed_point_count[label_mask_i-1] += 1;
          segmented_object_clouds[label_mask_i]->points.push_back(point);
          segmented_observed_point_count[label_mask_i] += 1;
        }
      }

      // Print total number of points in different segmented point clouds
      if (env_params_.use_external_pose_list == 1) 
      {        
        for (int i = 0; i < segmented_object_names.size(); i++)
        {
          printf("Segmented point count for label %s : %f\n", segmented_object_names[i].c_str(), segmented_observed_point_count[i]);
        }
      }
      if (env_params_.use_external_pose_list == 1)
      {
        // Below used in SetObservation() for downsampling on CPU
        // CPU Downsampling will be needed if ICP is on CPU
        original_input_cloud_ = depth_img_cloud;
      }
      else
      {
        // For 3-DOF, cloud on CPU has to be in reference/table frame but GPU gives it in camera frame
        // Conver to table frame and store it
        Eigen::Affine3f transform;
        transform.matrix() = *camera_transform_ptr;
        PointCloudPtr temp(new PointCloud);
        transformPointCloud(*depth_img_cloud, *original_input_cloud_,
                    transform);
      }
    }
    else
    {
      // If reading images on CPU, then use the GPU function to convert to clouds
      // Only tested for 3-Dof
      printf("Reading input images on CPU\n");
      depth_img_cloud = GetGravityAlignedPointCloudCV(cv_depth_image, cv_input_color_image, cv_predicted_mask_image, input.depth_factor);
      original_input_cloud_ = depth_img_cloud;
    }

  }
  else {
    // This is run when input images are not being used but input comes as cloud from robot or bag file
    *original_input_cloud_ = input.cloud;
    std::cout << "Set Input Camera Pose" << endl;
    Eigen::Affine3f cam_to_body;
    // Rotate things by 90 to put in camera optical frame
    cam_to_body.matrix() << 0, 0, 1, 0,
                      -1, 0, 0, 0,
                      0, -1, 0, 0,
                      0, 0, 0, 1;
    // PointCloudPtr depth_img_cloud(new PointCloud);
    Eigen::Affine3f transform;
    transform.matrix() = input.camera_pose.matrix().cast<float>();
    transform = cam_to_body.inverse() * transform.inverse();

    // transforming to camera frame to get depth image
    transformPointCloud(input.cloud, *depth_img_cloud,
                        transform);

    depth_image =
      sbpl_perception::OrganizedPointCloudToKinectDepthImage(depth_img_cloud);
  }

  *observed_organized_cloud_ = *depth_img_cloud;

  // Write input depth image to folder
  SetObservation(input.model_names.size(), depth_image);

  // Printpoint input cloud after downsampling etc.
  if (!perch_params_.use_gpu || !input.use_input_images) {
    if (IsMaster(mpi_comm_)) {
      for (int i = 0; i < 3; i++)
        PrintPointCloud(observed_cloud_, 1, input_point_cloud_topic);
    }
  }
  if (perch_params_.use_gpu && !input.use_input_images)
  {
    // Used when point cloud comes directly from robot or bag file and we need to use GPU code
    // Point clouds need to be converted to GPU formats

    printf("Copying PCL cloud to array for GPU\n");

    // observed_cloud_ is the downsampled input point cloud
    // CPU downsampling has to be used to downsample input cloud in this case as image stride based GPU cant be used  
    observed_point_num = observed_cloud_->points.size();
    printf("Observed input cloud point count : %d\n", observed_point_num);

    // Assign variables that can be copied to GPU
    result_observed_cloud = (float*) malloc(gpu_point_dim * observed_point_num * sizeof(float));
    result_observed_cloud_eigen = (Eigen::Vector3f*) malloc(observed_point_num * sizeof(Eigen::Vector3f));
    result_observed_cloud_color = (uint8_t*) malloc(gpu_point_dim * observed_point_num * sizeof(uint8_t));
    
    //// Need to transform back to camera frame for GPU version as KNN happens in cloud in camera frame
    Eigen::Isometry3d transform;
    Eigen::Isometry3d cam_to_body;
    cam_to_body.matrix() << 0, 0, 1, 0,
                      -1, 0, 0, 0,
                      0, -1, 0, 0,
                      0, 0, 0, 1;
    transform = cam_to_world_ * cam_to_body;
    PointCloudPtr transformed_cloud(new PointCloud);
    transformPointCloud (*observed_cloud_, *transformed_cloud, transform.matrix().inverse().cast<float>());

    // Loop to convert PCL cloud to array
    for (int i = 0; i < observed_point_num; i++)
    {
      PointT point = transformed_cloud->points[i];
      Eigen::Vector3f eig_point;
      uint32_t rgb = *reinterpret_cast<int*>(&point.rgb);

      result_observed_cloud[i + 0*observed_point_num] = point.x;
      result_observed_cloud[i + 1*observed_point_num] = point.y;
      result_observed_cloud[i + 2*observed_point_num] = point.z;

      eig_point[0] = point.x;
      eig_point[1] = point.y;
      eig_point[2] = point.z;

      result_observed_cloud_color[i + 0*observed_point_num] = (rgb >> 16);
      result_observed_cloud_color[i + 1*observed_point_num] = (rgb >> 8);
      result_observed_cloud_color[i + 2*observed_point_num] = rgb;

      result_observed_cloud_eigen[i] = eig_point;
    }

    // Need depth image format to do occlusion handling while rendering on GPU
    input_depth_image_vec.resize(kCameraHeight*kCameraWidth, 0);
    for (int ii = 0; ii < kCameraHeight; ++ii) {
      for (int jj = 0; jj < kCameraWidth; ++jj) {
        PointT p = depth_img_cloud->at(jj, ii);
        if (isnan(p.z) || isinf(p.z)) {
          input_depth_image_vec[ii * kCameraWidth + jj] = kKinectMaxDepth;
        } else {
          input_depth_image_vec[ii * kCameraWidth + jj] = static_cast<int>
                                                    (p.z * gpu_depth_factor);
        }
      }
    }
  }

  // Precompute RCNN heuristics.
  if (depth_image.size() > 0)
  {
    rcnn_heuristic_factory_.reset(new RCNNHeuristicFactory(input, original_input_cloud_,
                                                          kinect_simulator_));
    rcnn_heuristic_factory_->SetDebugDir(debug_dir_);

    if (perch_params_.use_rcnn_heuristic) {
      rcnn_heuristic_factory_->LoadHeuristicsFromDisk(input.heuristics_dir);
      rcnn_heuristics_ = rcnn_heuristic_factory_->GetHeuristics();
    }
  }
  end = chrono::system_clock::now();
  chrono::duration<double> elapsed_seconds = end-start;
  cout << "SetInput() took " << elapsed_seconds.count() << " seconds\n";

}

void EnvObjectRecognition::Initialize(const EnvConfig &env_config) {
  model_bank_ = env_config.model_bank;
  env_params_.res = env_config.res;
  env_params_.theta_res = env_config.theta_res;
  // TODO: Refactor.
  WorldResolutionParams world_resolution_params;
  SetWorldResolutionParams(env_params_.res, env_params_.res,
                           env_params_.theta_res, 0.0, 0.0, world_resolution_params);
  DiscretizationManager::Initialize(world_resolution_params);

  if (IsMaster(mpi_comm_)) {
    printf("----------Env Config-------------\n");
    printf("Translation resolution: %f\n", env_params_.res);
    printf("Rotation resolution: %f\n", env_params_.theta_res);
    printf("Mesh in millimeters: %d\n", kMeshInMillimeters);
    printf("Mesh scaling factor: %f\n", kMeshScalingFactor);
  }
}


double EnvObjectRecognition::GetICPAdjustedPose(const PointCloudPtr cloud_in,
                                                const ContPose &pose_in, PointCloudPtr &cloud_out, ContPose *pose_out,
                                                const std::vector<int> counted_indices /*= std::vector<int>(0)*/,
                                                const PointCloudPtr target_cloud,
                                                const std::string object_name) {
  /*
  * PCL ICP on CPU, mainly used for 3-Dof cases (works for 6-Dof though but not tested recently)
  * For 3-Dof ICP is done with whole input, for 6-Dof with segmented input cloud
  */
  if (cost_debug_msgs)
    printf("GetICPAdjustedPose()\n");
  auto start = std::chrono::high_resolution_clock::now();

  if (env_params_.use_icp == 0)
  {
    printf("No ICP done\n");
    cloud_out = cloud_in;
    *pose_out = pose_in;
    return 100;
  }

  pcl::IterativeClosestPointNonLinear<PointT, PointT> icp;
  // pcl::GeneralizedIterativeClosestPoint<PointT, PointT> icp;

  // int num_points_original = cloud_in->points.size();

  // if (cloud_in->points.size() > 2000) { //TODO: Fix it

  if (false) {
    PointCloudPtr cloud_in_downsampled = DownsamplePointCloud(cloud_in);
    icp.setInputSource(cloud_in_downsampled);
  } else {
    icp.setInputSource(cloud_in);
  }

  // Trying to account for lack of depth points in fish can
  float max_correspondence = perch_params_.icp_max_correspondence;
  if (object_name.size() > 0)
  {
    if (object_name.compare("007_tuna_fish_can") == 0)
    {
      if (cost_debug_msgs)
        printf("Resetting ICP max correspondence\n");
      max_correspondence = 0.08;
    }
  }

  if (target_cloud == NULL)
  {
    
    PointCloudPtr remaining_observed_cloud;
    if (counted_indices.size() > 0)
    {
      remaining_observed_cloud = perception_utils::IndexFilter(
                                                      observed_cloud_, counted_indices, true);
    }
    else
    {
      remaining_observed_cloud = observed_cloud_;
    }
    const PointCloudPtr remaining_downsampled_observed_cloud =
      DownsamplePointCloud(remaining_observed_cloud);
    icp.setInputTarget(remaining_downsampled_observed_cloud);
  }
  else
  {
    if (cost_debug_msgs)
    {
      printf("Target cloud size : %d\n", target_cloud->points.size());
      printf("Source cloud size : %d\n", cloud_in->points.size());
    }
    icp.setInputTarget(target_cloud);
  }
  
  // icp.setInputTarget(downsampled_observed_cloud_);
  // icp.setInputTarget(observed_cloud_);

  if (env_params_.use_external_pose_list == 0)
  {
    // 3Dof case, restrict degrees of freedom for speedup
    pcl::registration::TransformationEstimation2D<PointT, PointT>::Ptr est;
    est.reset(new pcl::registration::TransformationEstimation2D<PointT, PointT>);
    icp.setTransformationEstimation(est);
  }

  // TODO: make all of the following algorithm parameters and place in a config file.
  // Set the max correspondence distance (e.g., correspondences with higher distances will be ignored)
  icp.setMaxCorrespondenceDistance(max_correspondence);
  // Set the maximum number of iterations (criterion 1)
  icp.setMaximumIterations(perch_params_.max_icp_iterations);
  // Set the transformation epsilon (criterion 2)
  icp.setTransformationEpsilon(1e-8); //1e-8
  // Set the euclidean distance difference epsilon (criterion 3)
  // icp.setEuclideanFitnessEpsilon(perch_params_.sensor_resolution);  // 1e-5
  icp.setEuclideanFitnessEpsilon(1e-5);  // 1e-5
  // icp.setEuclideanFitnessEpsilon(0.0075);  // 1e-5

  icp.align(*cloud_out);
  double score = 100.0;

  if (icp.hasConverged()) {
    score = icp.getFitnessScore();
    Eigen::Matrix4f transformation = icp.getFinalTransformation();
    // cout << "pcl transform " << transformation << endl;
    // cout << "pcl fitness " << score << endl;

    Eigen::Matrix4f transformation_old = pose_in.GetTransform().matrix().cast<float>();
    Eigen::Matrix4f transformation_new = transformation * transformation_old;
    Eigen::Vector4f vec_out;

    if (env_params_.use_external_pose_list == 0)
    {
      // Apply only x,y,yaw
      Eigen::Vector4f vec_in;
      vec_in << pose_in.x(), pose_in.y(), pose_in.z(), 1.0;
      vec_out = transformation * vec_in;
      double yaw = atan2(transformation(1, 0), transformation(0, 0));

      double yaw1 = pose_in.yaw();
      double yaw2 = yaw;
      double cos_term = cos(yaw1 + yaw2);
      double sin_term = sin(yaw1 + yaw2);
      double total_yaw = atan2(sin_term, cos_term);
      *pose_out = ContPose(vec_out[0], vec_out[1], vec_out[2], pose_in.roll(), pose_in.pitch(), total_yaw);
    }
    else
    {
      // Convert to translation, euler representation for storing
      vec_out << transformation_new(0,3), transformation_new(1,3), transformation_new(2,3), 1.0;
      Matrix3f rotation_new(3,3);
      for (int i = 0; i < 3; i++){
        for (int j = 0; j < 3; j++){
          rotation_new(i, j) = transformation_new(i, j);
        }
      }
      auto euler = rotation_new.eulerAngles(2,1,0);
      double roll_ = euler[0];
      double pitch_ = euler[1];
      double yaw_ = euler[2];

      Quaternionf quaternion(rotation_new.cast<float>());

      *pose_out = ContPose(vec_out[0], vec_out[1], vec_out[2], quaternion.x(), quaternion.y(), quaternion.z(), quaternion.w());

      // *pose_out = pose_in;
    }

  } else {
    std::cout << "GetICPAdjustedPose() didnt converge\n";
    cloud_out = cloud_in;
    *pose_out = pose_in;
  }

  auto finish = std::chrono::high_resolution_clock::now();

  if (cost_debug_msgs)
    std::cout << "GetICPAdjustedPose() took "
              << std::chrono::duration_cast<std::chrono::milliseconds>(finish - start).count()
              << " milliseconds\n";

  return score;
}



double EnvObjectRecognition::GetVGICPAdjustedPose(const PointCloudPtr cloud_in,
                                                const ContPose &pose_in, PointCloudPtr &cloud_out, ContPose *pose_out,
                                                const std::vector<int> counted_indices /*= std::vector<int>(0)*/,
                                                const PointCloudPtr target_cloud,
                                                const std::string object_name) {
  /*
  * GICP on CPU, mainly used for 6-Dof cases (not tested on 3-Dof)
  * Calls the corresponding function from fast_gicp library
  * For 6-Dof done with segmented input cloud
  */
  if (cost_debug_msgs)
    printf("GetVGICPAdjustedPose()\n");



  ///////////////////////////////
  auto start_v = std::chrono::high_resolution_clock::now();
  fast_gicp::FastGICP<pcl::PointXYZ, pcl::PointXYZ> vgicp;
  // fast_gicp::FastVGICP<pcl::PointXYZ, pcl::PointXYZ> vgicp;
  pcl::PointCloud<pcl::PointXYZ>::Ptr vt(new pcl::PointCloud<pcl::PointXYZ>);
  pcl::PointCloud<pcl::PointXYZ>::Ptr vs(new pcl::PointCloud<pcl::PointXYZ>);
  // observed
  pcl::copyPointCloud(*cloud_in, *vs);
  // rendered
  pcl::copyPointCloud(*target_cloud, *vt);

  printf("Target cloud size : %d\n", vt->points.size());
  printf("Source cloud size : %d\n", vs->points.size());

  pcl::PointCloud<pcl::PointXYZ>::Ptr aligned(new pcl::PointCloud<pcl::PointXYZ>);
  // vgicp.setResolution(0.05);
  vgicp.setMaximumIterations(perch_params_.max_icp_iterations);
  vgicp.setCorrespondenceRandomness(10);
  // fast_gicp reuses calculated covariances if an input cloud is the same as the previous one
  // to prevent this for benchmarking, force clear source and target clouds
  vgicp.clearTarget();
  vgicp.clearSource();
  vgicp.setInputTarget(vt);
  vgicp.setInputSource(vs);
  vgicp.align(*aligned);
  cout << "vgicp mt tranform " << vgicp.getFinalTransformation() << endl;
  cout << "vgicp mt fitness " << vgicp.getFitnessScore() << endl;
  cout << "vgicp converged " << vgicp.hasConverged() << endl;
  Eigen::Matrix4f transformation = vgicp.getFinalTransformation();

  auto finish_v = std::chrono::high_resolution_clock::now();
  if (cost_debug_msgs)  
    std::cout << "GetVGICPAdjustedPose() took "
              << std::chrono::duration_cast<std::chrono::milliseconds>(finish_v - start_v).count()
              << " milliseconds\n";
  

  Eigen::Matrix4f transformation_old = pose_in.GetTransform().matrix().cast<float>();
  Eigen::Matrix4f transformation_new = transformation * transformation_old;
  Eigen::Vector4f vec_out;

  if (vgicp.hasConverged())
  {
    if (env_params_.use_external_pose_list == 0)
    {
      Eigen::Vector4f vec_in;
      vec_in << pose_in.x(), pose_in.y(), pose_in.z(), 1.0;
      vec_out = transformation * vec_in;
      double yaw = atan2(transformation(1, 0), transformation(0, 0));

      double yaw1 = pose_in.yaw();
      double yaw2 = yaw;
      double cos_term = cos(yaw1 + yaw2);
      double sin_term = sin(yaw1 + yaw2);
      double total_yaw = atan2(sin_term, cos_term);
      *pose_out = ContPose(vec_out[0], vec_out[1], vec_out[2], pose_in.roll(), pose_in.pitch(), total_yaw);
    }
    else
    {
      vec_out << transformation_new(0,3), transformation_new(1,3), transformation_new(2,3), 1.0;
      Matrix3f rotation_new(3,3);
      for (int i = 0; i < 3; i++){
        for (int j = 0; j < 3; j++){
          rotation_new(i, j) = transformation_new(i, j);
        }
      }
      auto euler = rotation_new.eulerAngles(2,1,0);
      double roll_ = euler[0];
      double pitch_ = euler[1];
      double yaw_ = euler[2];

      Quaternionf quaternion(rotation_new.cast<float>());

      *pose_out = ContPose(vec_out[0], vec_out[1], vec_out[2], quaternion.x(), quaternion.y(), quaternion.z(), quaternion.w());
    }
  }
  else
  {
    cloud_out = cloud_in;
    *pose_out = pose_in;
  }

}

// Feature-based and ICP Planners
GraphState EnvObjectRecognition::ComputeGreedyICPPoses() {
  // BF-ICP baseline code
  // We will slide the 'n' models in the database over the scene, and take the 'k' best matches.
  // The order of objects matters for collision checking--we will 'commit' to the best pose
  // for an object and disallow it for future objects.
  // ICP error is computed over full model (not just the non-occluded points)--this means that the
  // final score is always an upper bound
  chrono::time_point<chrono::system_clock> start, end;
  start = chrono::system_clock::now();

  int num_models = env_params_.num_models;
  vector<int> model_ids(num_models);

  for (int ii = 0; ii < num_models; ++ii) {
    model_ids[ii] = ii;
  }

  vector<double> permutation_scores;
  vector<GraphState> permutation_states;

  int succ_id = 0;

  if (env_params_.use_external_pose_list != 1)
  {
    do {
      vector<double> icp_scores; //smaller, the better
      vector<ContPose> icp_adjusted_poses;
      // icp_scores.resize(env_params_.num_models, numeric_limits<double>::max());
      icp_scores.resize(env_params_.num_models, 100.0);
      icp_adjusted_poses.resize(env_params_.num_models);

      GraphState empty_state;
      GraphState committed_state;
      vector<GraphState> candidate_succs;
      GenerateSuccessorStates(empty_state, &candidate_succs);

      env_stats_.scenes_rendered += static_cast<int>(candidate_succs.size());

      double total_score = 0;
      for (int model_id : model_ids) {

        auto model_bank_it = model_bank_.find(obj_models_[model_id].name());
        assert (model_bank_it != model_bank_.end());
        const auto &model_meta_data = model_bank_it->second;
        double search_resolution = 0;

        if (perch_params_.use_model_specific_search_resolution) {
          search_resolution = model_meta_data.search_resolution;
        } else {
          search_resolution = env_params_.res;
        }

        cout << "Greedy ICP for model: " << model_id << endl;
        int model_succ_count = 0;

        // Do ICP in parallel on CPU
        #pragma omp parallel for
        for (int ii = 0; ii < candidate_succs.size(); ii++) {
          ObjectState succ_obj_state = 
            candidate_succs[ii].object_states()[candidate_succs[ii].object_states().size() - 1];

          int succ_model_id = succ_obj_state.id();
          if (succ_model_id != model_id) continue;

          ContPose p_in = succ_obj_state.cont_pose();
          ContPose p_out = p_in;

          GraphState succ_state;
          const ObjectState object_state(model_id, obj_models_[model_id].symmetric(),
                                        p_in);
          succ_state.AppendObject(object_state);

          if (!IsValidPose(committed_state, model_id, p_in)) {
            continue;
          }

          auto transformed_mesh = obj_models_[model_id].GetTransformedMesh(p_in);
          PointCloudPtr cloud_in(new PointCloud);
          PointCloudPtr cloud_aligned(new PointCloud);
          pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_in_xyz (new
                                                            pcl::PointCloud<pcl::PointXYZ>);

          pcl::fromPCLPointCloud2(transformed_mesh->cloud, *cloud_in_xyz);
          copyPointCloud(*cloud_in_xyz, *cloud_in);
          if (perch_params_.use_downsampling) {
            cloud_in = DownsamplePointCloud(cloud_in, perch_params_.downsampling_leaf_size);
          }

          double icp_fitness_score = GetICPAdjustedPose(cloud_in, p_in,
                                                        cloud_aligned, &p_out);

          // Check *after* icp alignment
          if (!IsValidPose(committed_state, model_id, p_out)) {
            continue;
          }

          const auto old_state = succ_state.object_states()[0];
          succ_state.mutable_object_states()[0] = ObjectState(old_state.id(),
                                                              old_state.symmetric(), p_out);

          if (perch_params_.vis_expanded_states) {
            string fname = debug_dir_ + "succ_" + to_string(succ_id) + ".png";
            PrintState(succ_state, fname);
            printf("%d: %f\n", succ_id, icp_fitness_score);
          }

          if (icp_fitness_score < icp_scores[model_id]) {
            icp_scores[model_id] = icp_fitness_score;
            icp_adjusted_poses[model_id] = p_out;
            total_score += icp_fitness_score;
          }

          succ_id++;
          model_succ_count++;
        }
        printf("Processed %d succs for model %d\n", model_succ_count, model_id);
        committed_state.AppendObject(ObjectState(model_id,
                                                obj_models_[model_id].symmetric(),
                                                icp_adjusted_poses[model_id]));
      }

      permutation_scores.push_back(total_score);
      permutation_states.push_back(committed_state);
      // Try different permutations of objects
    } while (std::next_permutation(model_ids.begin(), model_ids.end()));
  }
  else
  {

    //// With 6-Dof - Not tested in a while
    vector<double> icp_scores; //smaller, the better
    vector<ContPose> icp_adjusted_poses;
    // icp_scores.resize(env_params_.num_models, numeric_limits<double>::max());
    icp_scores.resize(env_params_.num_models, 100.0);
    icp_adjusted_poses.resize(env_params_.num_models);

    GraphState empty_state;
    GraphState committed_state;
    double total_score = 0;
    // #pragma omp parallel for
    for (int model_id : model_ids) {

      auto model_bank_it = model_bank_.find(obj_models_[model_id].name());
      assert (model_bank_it != model_bank_.end());
      const auto &model_meta_data = model_bank_it->second;
      double search_resolution = 0;

      if (perch_params_.use_model_specific_search_resolution) {
        search_resolution = model_meta_data.search_resolution;
      } else {
        search_resolution = env_params_.res;
      }

      cout << "Greedy ICP for model: " << model_id << endl;
      int model_succ_count = 0;

      printf("States for model : %s\n", obj_models_[model_id].name().c_str());
      string render_states_dir;
      string render_states_path = "/media/aditya/A69AFABA9AFA85D9/Cruzr/code/DOPE/catkin_ws/src/perception/sbpl_perception/data/YCB_Video_Dataset/rendered";
      string render_states_path_parent = render_states_path;
      std::stringstream ss;
      ss << render_states_path << "/" << obj_models_[model_id].name();
      render_states_dir = ss.str();
      std::stringstream sst;
      sst << render_states_path << "/" << obj_models_[model_id].name() << "/" << "poses.txt";
      render_states_path = sst.str();
      // printf("Path for rendered states : %s\n", render_states_path.c_str());

      std::ifstream file(render_states_path);

      std::vector<std::vector<std::string> > dataList;

      std::string line = "";
      while (getline(file, line))
      {
          std::vector<std::string> vec;
          boost::algorithm::split(vec, line, boost::is_any_of(" "));
          // cout << vec.size();
          std::vector<double> doubleVector(vec.size());

          std::transform(vec.begin(), vec.end(), doubleVector.begin(), [](const std::string& val)
          {
              return std::stod(val);
          });
          ContPose p_in(
            doubleVector[0], doubleVector[1], doubleVector[2], doubleVector[3], doubleVector[4], doubleVector[5], doubleVector[6]
          );
          ContPose p_out = p_in;
          GraphState succ_state;
          const ObjectState object_state(model_id, obj_models_[model_id].symmetric(),
                                        p_in);
          succ_state.AppendObject(object_state);

          if (!IsValidPose(committed_state, model_id, p_in)) {
            continue;
          }

          auto transformed_mesh = obj_models_[model_id].GetTransformedMesh(p_in);
          PointCloudPtr cloud_in(new PointCloud);
          PointCloudPtr cloud_aligned(new PointCloud);
          pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_in_xyz (new
                                                            pcl::PointCloud<pcl::PointXYZ>);

          pcl::fromPCLPointCloud2(transformed_mesh->cloud, *cloud_in_xyz);
          copyPointCloud(*cloud_in_xyz, *cloud_in);
          if (perch_params_.use_downsampling) {
            cloud_in = DownsamplePointCloud(cloud_in, perch_params_.downsampling_leaf_size);
          }

          double icp_fitness_score = GetICPAdjustedPose(cloud_in, p_in,
                                                        cloud_aligned, &p_out);

          // Check *after* icp alignment
          if (!IsValidPose(committed_state, model_id, p_out)) {
            continue;
          }

          const auto old_state = succ_state.object_states()[0];
          succ_state.mutable_object_states()[0] = ObjectState(old_state.id(),
                                                              old_state.symmetric(), p_out);

          if (image_debug_) {
            string fname = debug_dir_ + "succ_" + to_string(succ_id) + ".png";
            PrintState(succ_state, fname);
            printf("%d: %f\n", succ_id, icp_fitness_score);
          }

          if (icp_fitness_score < icp_scores[model_id]) {
            icp_scores[model_id] = icp_fitness_score;
            icp_adjusted_poses[model_id] = p_out;
            total_score += icp_fitness_score;
          }
          succ_id++;
          model_succ_count++;

      } // all states for given model done
      printf("Processed %d succs for model %d\n", model_succ_count, model_id);
      committed_state.AppendObject(ObjectState(model_id,
                                                obj_models_[model_id].symmetric(),
                                                icp_adjusted_poses[model_id]));
    } // all models done
    permutation_scores.push_back(total_score);
    permutation_states.push_back(committed_state);

  }
  // Take the first 'k'
  auto min_element_it = std::min_element(permutation_scores.begin(),
                                         permutation_scores.end());
  int offset = std::distance(permutation_scores.begin(), min_element_it);

  GraphState greedy_state = permutation_states[offset];
  std::sort(greedy_state.mutable_object_states().begin(),
            greedy_state.mutable_object_states().end(), [](const ObjectState & state1,
  const ObjectState & state2) {
    return state1.id() < state2.id();
  });
  cout << "State from greedy ICP " << endl << greedy_state << endl;
  end = chrono::system_clock::now();
  chrono::duration<double> elapsed_seconds = end-start;
  env_stats_.time = elapsed_seconds.count();

  string fname = debug_dir_ + "depth_greedy_state.png";
  string cname = debug_dir_ + "color_greedy_state.png";
  PrintState(greedy_state, fname, cname);
  return greedy_state;
}

void EnvObjectRecognition::SetDebugOptions(bool image_debug) {
  image_debug_ = image_debug;
}

void EnvObjectRecognition::SetDebugDir(const string &debug_dir) {
  debug_dir_ = debug_dir;
}

const EnvStats &EnvObjectRecognition::GetEnvStats() {
  env_stats_.scenes_valid = hash_manager_.Size() - 1; // Ignore the start state
  return env_stats_;
}

void EnvObjectRecognition::GetGoalPoses(int true_goal_id,
                                        vector<ContPose> *object_poses) {
  object_poses->clear();

  GraphState goal_state;

  if (adjusted_states_.find(true_goal_id) != adjusted_states_.end()) {
    goal_state = adjusted_states_[true_goal_id];
  } else {
    goal_state = hash_manager_.GetState(true_goal_id);
  }

  assert(static_cast<int>(goal_state.NumObjects()) == env_params_.num_objects);
  object_poses->resize(env_params_.num_objects);

  for (int ii = 0; ii < env_params_.num_objects; ++ii) {
    auto it = std::find_if(goal_state.object_states().begin(),
    goal_state.object_states().end(), [ii](const ObjectState & object_state) {
      return object_state.id() == ii;
    });
    assert(it != goal_state.object_states().end());
    object_poses->at(ii) = it->cont_pose();
  }
}

vector<PointCloudPtr> EnvObjectRecognition::GetObjectPointClouds(
  const vector<int> &solution_state_ids) {
  // The solution state ids will also include the root state (with id 0) that has no
  // objects.
  assert(solution_state_ids.size() == env_params_.num_objects + 1);
  vector<PointCloudPtr> object_point_clouds;
  object_point_clouds.resize(env_params_.num_objects);

  vector<int> last_counted_indices;
  vector<int> delta_counted_indices;

  // Create Eigen types of input cloud.
  vector<Eigen::Vector3d> eig_points(original_input_cloud_->points.size());
  std::transform(original_input_cloud_->points.begin(),
                 original_input_cloud_->points.end(),
  eig_points.begin(), [](const PointT & pcl_point) {
    Eigen::Vector3d eig_point;
    eig_point[0] = pcl_point.x;
    eig_point[1] = pcl_point.y;
    eig_point[2] = pcl_point.z;
    return eig_point;
  });

  for (int ii = 1; ii < solution_state_ids.size(); ++ii) {
    int graph_state_id = solution_state_ids[ii];

    // Handle goal state differently.
    if (ii == solution_state_ids.size() - 1) {
      graph_state_id = GetBestSuccessorID(
                         solution_state_ids[solution_state_ids.size() - 2]);
    }

    GraphState graph_state;

    if (adjusted_states_.find(graph_state_id) != adjusted_states_.end()) {
      graph_state = adjusted_states_[graph_state_id];
    } else {
      graph_state = hash_manager_.GetState(graph_state_id);
    }

    int object_id = graph_state.object_states().back().id();
    assert(object_id >= 0 && object_id < env_params_.num_objects);

    const vector<bool> inside_mesh = obj_models_[object_id].PointsInsideMesh(
                                       eig_points, graph_state.object_states().back().cont_pose());
    vector<int> inside_mesh_indices;
    inside_mesh_indices.reserve(inside_mesh.size());

    for (size_t ii = 0; ii < inside_mesh.size(); ++ii) {
      if (inside_mesh[ii]) {
        inside_mesh_indices.push_back(static_cast<int>(ii));
      }
    }

    PointCloudPtr object_cloud = perception_utils::IndexFilter(
                                   original_input_cloud_, inside_mesh_indices, false);
    object_point_clouds[object_id] = object_cloud;


  }

  return object_point_clouds;
}

// void EnvObjectRecognition::GetShiftedCentroidPosesGPU(const vector<ObjectState>& objects,
//                                                       vector<ObjectState>& modified_objects,
//                                                       int start_index)
// {
  // Deprecated
  /*
   * Render once and realign the centroid with the centroid in the pose
   * Currently renders by using the input depth image to handle occlusion
   * Uses min/max of rendered point cloud to get centroid
   */
  // int num_poses = objects.size();
  // printf("GetShiftedCentroidPosesGPU() for %d poses, batch index : %d\n", num_poses, start_index);
  // std::vector<int> random_poses_occluded(num_poses, 0);
  // std::vector<int> random_poses_occluded_other(num_poses, 0);
  // std::vector<float> random_pose_clutter_cost(num_poses, 0);

  // // ObjectState temp;
  // // modified_objects.resize(num_poses, temp);

  // std::vector<std::vector<uint8_t>> random_color(3);
  // std::vector<uint8_t> random_red(kCameraWidth * kCameraHeight, 255);
  // std::vector<uint8_t> random_green(kCameraWidth * kCameraHeight, 255);
  // std::vector<uint8_t> random_blue(kCameraWidth * kCameraHeight, 255);
  // random_color[0] = random_red;
  // random_color[1] = random_green;
  // random_color[2] = random_blue;
  // std::vector<int32_t> random_depth(kCameraWidth * kCameraHeight, 0);
  // std::vector<int32_t> source_result_depth(kCameraWidth * kCameraHeight, 0);

  // std::vector<std::vector<uint8_t>> result_color;
  // std::vector<int32_t> result_depth;
  // vector<int> pose_segmentation_label;
  // vector<float> pose_observed_points_total;

  // // if (env_params_.use_external_pose_list == 1)
  // // {
  // // vector<ObjectState> render_object_states;
  // // for (ObjectState state : objects)
  // // {
  // //   ContPose pose_in = state.cont_pose();
  // //   ContPose pose_out = 
  // //     ContPose(0, 0, pose_in.z(), pose_in.qx(), pose_in.qy(), pose_in.qz(), pose_in.qw());

  // //   ObjectState modified_object_state(state.id(), state.symmetric(), pose_out, state.segmentation_label_id());
  // //   render_object_states.push_back(modified_object_state);
  // // }

  // // GetStateImagesGPU(
  // //   objects, random_color, source_result_depth, 
  // //   result_color, result_depth, random_poses_occluded, 0,
  // //   random_poses_occluded_other, random_pose_clutter_cost,
  // //   pose_segmentation_label
  // // );
  

  // float* result_cloud;
  // uint8_t* result_cloud_color;
  // int* dc_index;
  // int* cloud_pose_map;
  // // int32_t* depth_data = result_depth.data();
  // int rendered_point_num;
  // // int* poses_occluded_ptr = random_poses_occluded.data();
  // // int* cloud_label;

  // // cuda_renderer::depth2cloud_global(
  // //       depth_data, result_color, result_cloud, result_cloud_color, dc_index, rendered_point_num, cloud_pose_map, env_params_.width, env_params_.height, 
  // //       num_poses, poses_occluded_ptr, kCameraCX, kCameraCY, kCameraFX, kCameraFY, gpu_depth_factor, gpu_stride, gpu_point_dim, cloud_label
  // //     );
  
  // float* rendered_cost;
  // float* observed_cost;
  // float* points_diff_cost_gpu;

  // string state = "CLOUD";
  // if (perch_params_.vis_successors)
  // {
  //   state = "DEBUG";
  // }
  // // env_params_.height *= 1.5;
  // // env_params_.width *= 1.5;
  // GetStateImagesUnifiedGPU(
  //   state,
  //   objects,
  //   // render_object_states,
  //   random_color,
  //   // source_result_depth,
  //   random_depth,
  //   result_color,
  //   result_depth,
  //   0,
  //   random_pose_clutter_cost,
  //   result_cloud,
  //   result_cloud_color,
  //   rendered_point_num,
  //   dc_index,
  //   cloud_pose_map,
  //   rendered_cost,
  //   observed_cost,
  //   points_diff_cost_gpu,
  //   perch_params_.sensor_resolution
  // );

  
  // if (perch_params_.vis_successors)
  // {
  //   PrintGPUImages(result_depth, result_color, num_poses, "succ_pre_shift", random_poses_occluded);
  // }
  // // env_params_.height = kCameraHeight;
  // // env_params_.width = kCameraWidth;
  // vector<float> pose_centroid_x(num_poses, 0.0);
  // vector<float> pose_centroid_y(num_poses, 0.0);
  // vector<float> pose_centroid_z(num_poses, 0.0);
  // vector<float> pose_total_points(num_poses, 0.0);
  // vector<float> min_x(num_poses, INT_MAX);
  // vector<float> max_x(num_poses, INT_MIN);
  // vector<float> min_y(num_poses, INT_MAX);
  // vector<float> max_y(num_poses, INT_MIN);
  // vector<float> min_z(num_poses, INT_MAX);
  // vector<float> max_z(num_poses, INT_MIN);
  // for(int cloud_index = 0; cloud_index < rendered_point_num; cloud_index++)
  // {
  //     int pose_index = cloud_pose_map[cloud_index];
  //     // pose_centroid_x[pose_index] += result_cloud[cloud_index + 0*rendered_point_num];
  //     // pose_centroid_y[pose_index] += result_cloud[cloud_index + 1*rendered_point_num];
  //     // pose_centroid_z[pose_index] += result_cloud[cloud_index + 2*rendered_point_num];
  //     // pose_total_points[pose_index] += 1.0;

  //     min_x[pose_index] = std::min(min_x[pose_index], result_cloud[cloud_index + 0*rendered_point_num]);
  //     max_x[pose_index] = std::max(max_x[pose_index], result_cloud[cloud_index + 0*rendered_point_num]);
  //     min_y[pose_index] = std::min(min_y[pose_index], result_cloud[cloud_index + 1*rendered_point_num]);
  //     max_y[pose_index] = std::max(max_y[pose_index], result_cloud[cloud_index + 1*rendered_point_num]);
  //     min_z[pose_index] = std::min(min_z[pose_index], result_cloud[cloud_index + 2*rendered_point_num]);
  //     max_z[pose_index] = std::max(max_z[pose_index], result_cloud[cloud_index + 2*rendered_point_num]);
  //     // for(int i = 0; i < env_params_.height; i = i + stride)
  //     // {
  //     //   for(int j = 0; j < env_params_.width; j = j + stride)
  //     //   {
  //     //     int index = n*env_params_.width*env_params_.height + (i*env_params_.width + j);
  //     //     int cloud_index = dc_index[index];
  //     //   }
  //     // }
  // }
  // // std::transform(pose_centroid_x.begin(), pose_centroid_x.end(), pose_centroid_x.begin(), pose_centroid_x.begin(), std::divides<float>()); 
  // // std::transform(pose_centroid_x.begin(), pose_centroid_y.end(), pose_total_points.begin(), pose_centroid_y.begin(), std::divides<float>()); 
  // // std::transform(pose_centroid_x.begin(), pose_centroid_z.end(), pose_total_points.begin(), pose_centroid_z.begin(), std::divides<float>()); 
  // for (int i = 0; i < num_poses; i++)
  // {
  //   // pose_centroid_x[i] /= pose_total_points[i];
  //   // pose_centroid_y[i] /= pose_total_points[i];
  //   // pose_centroid_z[i] /= pose_total_points[i];

  //   pose_centroid_x[i] = (max_x[i] + min_x[i])/2;
  //   pose_centroid_y[i] = (max_y[i] + min_y[i])/2;
  //   pose_centroid_z[i] = (max_z[i] + min_z[i])/2;
    
  //   ContPose pose_in = objects[i].cont_pose();
  //   // printf("Centroid old : %f,%f,%f\n", pose_in.x(), pose_in.y(), pose_in.z());
  //   // printf("Centroid new : %f,%f,%f\n", pose_centroid_x[i], pose_centroid_y[i], pose_centroid_z[i]);
    
    
  //   float x_diff = pose_in.x()-pose_centroid_x[i];
  //   float y_diff = pose_in.y()-pose_centroid_y[i];
  //   float z_diff = pose_in.z()-pose_centroid_z[i];
  //   printf("Centroid diff for pose %d : %f,%f,%f\n", i, x_diff, y_diff, z_diff);

    
  //   ContPose pose_out = 
  //     ContPose(pose_in.x() + x_diff, pose_in.y() + y_diff, pose_in.z() + z_diff, pose_in.qx(), pose_in.qy(), pose_in.qz(), pose_in.qw());

  //   ObjectState modified_object_state(objects[i].id(), objects[i].symmetric(), pose_out, objects[i].segmentation_label_id());

  //   // modified_objects.push_back(modified_object_state);
  //   modified_objects[i + start_index] = modified_object_state;

  // }

  // // result_depth.clear();
  // // result_color.clear();
  // // GetStateImagesGPU(
  // //     modified_objects, random_color, random_depth, 
  // //     result_color, result_depth, random_poses_occluded, 0,
  // //     random_poses_occluded_other
  // //   );
  
  // // PrintGPUImages(result_depth, result_color, num_poses, "succ_post_shift", random_poses_occluded);
// }

void EnvObjectRecognition::GenerateSuccessorStates(const GraphState
                                                   &source_state, std::vector<GraphState> *succ_states) {

  printf("GenerateSuccessorStates() \n");
  assert(succ_states != nullptr);
  succ_states->clear();

  const auto &source_object_states = source_state.object_states();

  for (int ii = 0; ii < env_params_.num_models; ++ii) {
    // Find object state corresponding to this ii
    auto it = std::find_if(source_object_states.begin(),
    source_object_states.end(), [ii](const ObjectState & object_state) {
      return object_state.id() == ii;
    });

    if (it != source_object_states.end()) {
      printf("ERROR:Didnt generate any successor\n");
      continue;
    }

    auto model_bank_it = model_bank_.find(obj_models_[ii].name());
    assert (model_bank_it != model_bank_.end());
    const auto &model_meta_data = model_bank_it->second;
    double search_resolution = 0;

    if (perch_params_.use_model_specific_search_resolution) {
      search_resolution = model_meta_data.search_resolution;
    } else {
      search_resolution = env_params_.res;
    }

    const double res = perch_params_.use_adaptive_resolution ?
                       obj_models_[ii].GetInscribedRadius() : search_resolution;

    if (env_params_.use_external_render == 1 || env_params_.use_external_pose_list == 1)
    {
        // For 6-Dof poses are read from the file written by Python
        vector<ObjectState> unshifted_object_states, shifted_object_states;
        printf("States for model : %s\n", obj_models_[ii].name().c_str());
        if (source_state.object_states().size() == 0)
        {
          string render_states_dir;
          string render_states_path = env_params_.rendered_root_dir;
          string render_states_path_parent = render_states_path;
          std::stringstream ss;
          ss << render_states_path << "/" << obj_models_[ii].name();
          render_states_dir = ss.str();
          std::stringstream sst;
          sst << render_states_path << "/" << obj_models_[ii].name() << "/" << "poses.txt";
          render_states_path = sst.str();
          // printf("Path for rendered states : %s\n", render_states_path.c_str());

          std::ifstream file(render_states_path);
          std::vector<std::vector<std::string> > dataList;
          std::string line = "";

          // Iterate through each line and split the content using delimeter
          int external_pose_id = 0;
          int succ_count = 0;
          while (getline(file, line))
          {
              std::vector<std::string> vec;
              boost::algorithm::split(vec, line, boost::is_any_of(" "));
              // cout << vec.size();
              std::vector<double> doubleVector(vec.size());

              std::transform(vec.begin(), vec.end(), doubleVector.begin(), [](const std::string& val)
              {
                  return std::stod(val);
              });

              ContPose p(doubleVector[0], doubleVector[1], doubleVector[2], doubleVector[3], doubleVector[4], doubleVector[5], doubleVector[6]);
              external_pose_id++;
              // std::cout << "File Pose : " << doubleVector[0] << " " <<
              //   doubleVector[1] <<  " " << doubleVector[2] <<  " " << doubleVector[3] <<  " " << doubleVector[4] << " " << doubleVector[5] << " " << endl;

              // std::cout << "Cont Pose : " << p << endl;

              // cout << doubleVector[0];
              int required_object_id = distance(segmented_object_names.begin(), 
                  find(segmented_object_names.begin(), segmented_object_names.end(), obj_models_[ii].name()));

              // Check for min points in neighbourhood of pose
              if (!IsValidPose(source_state, ii, p, false, required_object_id)) {
                continue;
              }


              GraphState s = source_state; // Can only add objects, not remove them
              const ObjectState new_object(ii, obj_models_[ii].symmetric(), p, required_object_id + 1);
              
              unshifted_object_states.push_back(new_object);
              succ_count += 1;
              
              if (!perch_params_.use_gpu)
              {
                if (env_params_.shift_pose_centroid == 0)
                  s.AppendObject(new_object);

                GraphState s_render;
                s_render.AppendObject(new_object);
                // if object states are same and in same order id will be same

                // int succ_id = hash_manager_.GetStateIDForceful(s_render);
                // printf("Succ id : %d\n", succ_id);

                if (env_params_.shift_pose_centroid == 1 || (perch_params_.vis_successors && s.object_states().size() == 1)) {
                  vector<unsigned short> depth_image, last_obj_depth_image;
                  cv::Mat last_cv_obj_depth_image;
                  vector<vector<unsigned char>> color_image, last_obj_color_image;
                  cv::Mat last_cv_obj_color_image;
                  int num_occluders = 0;
                  bool shift_pose_centroid = env_params_.shift_pose_centroid == 1 ? true : false;
                  GetDepthImage(s_render, &last_obj_depth_image, &last_obj_color_image,
                                                    last_cv_obj_depth_image, last_cv_obj_color_image, &num_occluders, shift_pose_centroid);

                  const auto shifted_object_state = s_render.object_states()[0];
                  s.AppendObject(shifted_object_state);
                  valid_succ_cache[ii].push_back(shifted_object_state);

                  std::string color_image_path, depth_image_path;
                  std::stringstream ss1;
                  ss1 << debug_dir_ << "/successor-" << obj_models_[ii].name() << "-" << succ_count << "-color.png";
                  color_image_path = ss1.str();
                  ss1.clear();
                  ss1 << debug_dir_ << "/successor-" << obj_models_[ii].name() << "-" << succ_count << "-depth.png";
                  depth_image_path = ss1.str();

                  if (s.object_states().size() == 1 && perch_params_.vis_successors)
                  {
                    // Write successors only once even if pruning is on
                    cv::imwrite(color_image_path, last_cv_obj_color_image);
                    if (IsMaster(mpi_comm_)) {
                      PointCloudPtr cloud_in = GetGravityAlignedPointCloud(last_obj_depth_image, last_obj_color_image);
                      PrintPointCloud(cloud_in, 1, render_point_cloud_topic);
                      // cv::imshow("valid image", last_cv_obj_color_image);
                    }
                  }
                }
                else
                {
                  valid_succ_cache[ii].push_back(new_object);
                }

                succ_states->push_back(s);
              }
          }
          // Close the File
          file.close();
          
          if (perch_params_.use_gpu)
          {
            if (unshifted_object_states.size() > 0)
            {
              shifted_object_states.resize(unshifted_object_states.size());
              //// Centroid shifting is not needed, so it has been commented 
              // if (env_params_.shift_pose_centroid == 1)
              // {
              //   int num_batches = unshifted_object_states.size()/perch_params_.gpu_batch_size + 1;
              //   printf("Num GPU batches for given batch size : %d\n", num_batches);
              //   for (int bi = 0; bi < num_batches; bi++)
              //   {
              //     int start_index = bi * perch_params_.gpu_batch_size;
              //     vector<ObjectState>::const_iterator batch_start = unshifted_object_states.begin() + start_index;
              //     int end_index = std::min((bi + 1) * perch_params_.gpu_batch_size, (int) unshifted_object_states.size());

              //     if (end_index > unshifted_object_states.size() || start_index >= unshifted_object_states.size()) break;

              //     vector<ObjectState>::const_iterator batch_end = unshifted_object_states.begin() + end_index;
              //     vector<ObjectState> batch_unshifted_object_states(batch_start, batch_end);
              //     printf("\n\nGetting shifted poses for GPU batch : %d, num poses : %d\n", bi, batch_unshifted_object_states.size());
  
              //     GetShiftedCentroidPosesGPU(batch_unshifted_object_states, shifted_object_states, start_index);
              //     batch_unshifted_object_states.clear();
              //   }

              //   valid_succ_cache[ii].clear();
              // }
              // else
              // {
              shifted_object_states = unshifted_object_states;
              // }
              // std::copy(shifted_object_states.begin(), shifted_object_states.end(), valid_succ_cache[ii].begin());
              for (size_t i = 0; i < shifted_object_states.size(); i++)
              {
                const ObjectState object_state = shifted_object_states[i];
                GraphState s = source_state; 
                s.AppendObject(object_state);
                succ_states->push_back(s);
                valid_succ_cache[ii].push_back(object_state);
              }
            }
            else
            {
              printf("No valid states found for this model\n");
            }
          }
        }
        else
        {
          printf("GenerateSuccessorStates() from cache\n");
          for (size_t i = 0; i < valid_succ_cache[ii].size(); i++)
          {
            // const ObjectState new_object(ii, obj_models_[ii].symmetric(), p);
            GraphState s = source_state; // Can only add objects, not remove them
            s.AppendObject(valid_succ_cache[ii][i]);
            succ_states->push_back(s);
          }
        }

    }
    else
    {
        // For 3-dof generate by discretizing x,y, yaw
        printf("States for model : %s\n", obj_models_[ii].name().c_str());
        int succ_count = 0;
        if (source_state.object_states().size() == 0)
        {
          // #pragma omp parallel for
          for (double x = env_params_.x_min; x <= env_params_.x_max;
              x += res) {
            // #pragma omp parallel for
            for (double y = env_params_.y_min; y <= env_params_.y_max;
                y += res) {
              // #pragma omp parallel for
              // for (double pitch = 0; pitch < M_PI; pitch+=M_PI/2) {
              for (double theta = 0; theta < 2 * M_PI; theta += env_params_.theta_res) {
                ContPose p(x, y, env_params_.table_height, 0.0, 0.00, theta);
                // ContPose p(x, y, env_params_.table_height, 0.0, 0.0, -theta);
                // if (succ_count == 20)
                // {
                //   break;
                // }
                //check valid poses need to check!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
                // if (!IsValidPose(source_state, ii, p)) {
                //   // std::cout << "Not a valid pose for theta : " << theta << " " << endl;
                // cout << "Pose " << p << endl;
                if (!IsValidPose(source_state, ii, p)) {
                  // std::cout << "Not a valid pose for theta : " << theta << " " << endl;

                  continue;
                }

                // If 180 degree symmetric, then iterate only between 0 and 180, break if going above.

                if (model_meta_data.symmetry_mode == 1 &&
                    theta > (M_PI + env_params_.theta_res)) {
                  printf("Semi-symmetric object\n");
                  break;
                }
                // std::cout << "Valid pose for theta : " << p << endl;

                GraphState s = source_state; // Can only add objects, not remove them
                const ObjectState new_object(ii, obj_models_[ii].symmetric(), p);
                s.AppendObject(new_object);


                // int succ_id = hash_manager_.GetStateIDForceful(s);
                // if object states are same and in same order id will be same
                // printf("Succ id : %d\n", succ_id);

                GraphState s_render;
                s_render.AppendObject(new_object);

                // succ_states->push_back(s);
                // bool kHistogramPruning = true;
                cv::Mat last_cv_obj_depth_image, last_cv_obj_color_image;
                vector<unsigned short> last_obj_depth_image;
                vector<vector<unsigned char>> last_obj_color_image;
                std::string color_image_path, depth_image_path;
                PointCloudPtr cloud_in;

                bool vis_successors_ = true;
                if ((perch_params_.vis_successors && s.object_states().size() == 1) || kUseHistogramPruning || kUseOctomapPruning)
                {
                  // Process successors once when only one object scene or when pruning is on (then it needs to be done always)
                  int num_occluders = 0;
                  GetDepthImage(s_render, &last_obj_depth_image, &last_obj_color_image,
                                                    last_cv_obj_depth_image, last_cv_obj_color_image, &num_occluders, false);
                  std::stringstream ss1;
                  ss1 << debug_dir_ << "/successor-" << obj_models_[ii].name() << "-" << succ_count << "-color.png";
                  color_image_path = ss1.str();
                  ss1.clear();
                  ss1 << debug_dir_ << "/successor-" << obj_models_[ii].name() << "-" << succ_count << "-depth.png";
                  depth_image_path = ss1.str();


                  if (kUseHistogramPruning)
                  {
                    double histogram_distance;
                    double score = 0.85;

                    if (IsValidHistogram(ii, last_cv_obj_color_image, score, histogram_distance))
                    {
                      if (s.object_states().size() == 1 && perch_params_.vis_successors)
                      {
                        // Write successors only once even if pruning is on
                        cv::imwrite(color_image_path, last_cv_obj_color_image);
                        if (IsMaster(mpi_comm_)) {
                          cloud_in = GetGravityAlignedPointCloud(last_obj_depth_image, last_obj_color_image);
                          PrintPointCloud(cloud_in, 1, render_point_cloud_topic);
                          // cv::imshow("valid image", last_cv_obj_color_image);
                        }
                      }
                      valid_succ_cache[ii].push_back(new_object);
                      succ_states->push_back(s);
                      succ_count += 1;
                    }
                  }

                  if (kUseOctomapPruning)
                  {
                    int num_points_changed = 0;
                    cloud_in = GetGravityAlignedPointCloud(last_obj_depth_image, last_obj_color_image);

                    const float resolution = 0.02;
                    pcl::octree::OctreePointCloudChangeDetector<pcl::PointXYZRGB> octree_sim (resolution);
                    octree_sim.setInputCloud (cloud_in);
                    octree_sim.addPointsFromInputCloud ();

                    octree_sim.switchBuffers ();

                    octree_sim.setInputCloud (observed_cloud_);
                    octree_sim.addPointsFromInputCloud ();

                    std::vector<int> newPointIdxVector;

                    octree_sim.getPointIndicesFromNewVoxels (newPointIdxVector);
                    num_points_changed = newPointIdxVector.size();
                    printf("Number of points changed : %d\n", newPointIdxVector.size());
                    printf("Fraction of points changed : %f\n", (float) num_points_changed/observed_cloud_->points.size());

                    if ((float) num_points_changed/observed_cloud_->points.size() < 0.8)
                    {
                      if (s.object_states().size() == 1 && perch_params_.vis_successors) {
                        cv::imwrite(color_image_path, last_cv_obj_color_image);
                        cv::imwrite(depth_image_path, last_cv_obj_depth_image);
                        if (IsMaster(mpi_comm_)) {
                          PrintPointCloud(cloud_in, 1, render_point_cloud_topic);
                        }
                      }
                      valid_succ_cache[ii].push_back(new_object);
                      succ_states->push_back(s);
                      succ_count += 1;
                    }
                  }


                }

                if (!kUseHistogramPruning && !kUseOctomapPruning)
                {
                  if (s.object_states().size() == 1 && perch_params_.vis_successors)
                  {
                    // Write successors only once
                    cv::imwrite(color_image_path, last_cv_obj_color_image);
                    cv::imwrite(depth_image_path, last_cv_obj_depth_image);

                    if (IsMaster(mpi_comm_)) {
                      cloud_in = GetGravityAlignedPointCloud(last_obj_depth_image, last_obj_color_image);
                      PrintPointCloud(cloud_in, 1, render_point_cloud_topic);
                      // cv::imshow("valid image", last_cv_obj_color_image);

                      // const float resolution = 0.02;
                      // // Instantiate octree-based point cloud change detection class
                      // pcl::octree::OctreePointCloudChangeDetector<pcl::PointXYZRGB> octree_sim (resolution);
                      // octree_sim.setInputCloud (cloud_in);
                      // octree_sim.addPointsFromInputCloud ();

                      // octree_sim.switchBuffers ();

                      // octree_sim.setInputCloud (observed_cloud_);
                      // octree_sim.addPointsFromInputCloud ();

                      // std::vector<int> newPointIdxVector;

                      // // Get vector of point indices from octree voxels which did not exist in previous buffer
                      // octree_sim.getPointIndicesFromNewVoxels (newPointIdxVector);
                      // num_points_changed = newPointIdxVector.size();
                      // printf("Number of points changed : %d\n", newPointIdxVector.size());
                      // printf("Fraction of points changed : %f\n", (float) num_points_changed/observed_cloud_->points.size());

                      // // uint8_t rgb[3] = {255,255,255};
                      // // for (size_t i = 0; i < newPointIdxVector.size (); ++i) {
                      // //   uint32_t rgbc = ((uint32_t)rgb[0] << 16 | (uint32_t)rgb[1] << 8 | (uint32_t)rgb[2]);
                      // //   observed_cloud_->points[newPointIdxVector[i]].rgb = *reinterpret_cast<float*>(&rgbc);

                      // //   // std::cout << i << "# Index:" << newPointIdxVector[i]
                      // //   //           << "  Point:" << cloudB->points[newPointIdxVector[i]].x << " "
                      // //   //           << cloudB->points[newPointIdxVector[i]].y << " "
                      // //   //           << cloudB->points[newPointIdxVector[i]].z << std::endl;
                      // // }
                      // // PrintPointCloud(cloud_in, 1, render_point_cloud_topic);
                      // if ((float) num_points_changed/observed_cloud_->points.size() < 0.8)
                      // {
                      //   PrintPointCloud(cloud_in, 1, render_point_cloud_topic);
                      // }
                    }
                  }
                  // if ((float) num_points_changed/observed_cloud_->points.size() < 0.8)
                  // {
                  valid_succ_cache[ii].push_back(new_object);
                  succ_states->push_back(s);
                  succ_count += 1;
                  // }
                }
                // printf("Object added  to state x:%f y:%f z:%f theta: %f \n", x, y, env_params_.table_height, theta);
                // If symmetric object, don't iterate over all theta
                // Break after adding first theta from above
                if (obj_models_[ii].symmetric() || model_meta_data.symmetry_mode == 2) {
                  break;
                }
              }
            }
          }
        }
        else
        {
          // No need to generate successors again if done before
          printf("GenerateSuccessorStates() from cache\n");
          for (size_t i = 0; i < valid_succ_cache[ii].size(); i++)
          {
            // const ObjectState new_object(ii, obj_models_[ii].symmetric(), p);
            GraphState s = source_state; // Can only add objects, not remove them
            s.AppendObject(valid_succ_cache[ii][i]);
            succ_states->push_back(s);
          }
        }

    }
  }

  std::cout << "Size of successor states : " << succ_states->size() << endl;
}

bool EnvObjectRecognition::GetComposedDepthImage(const vector<unsigned short> &source_depth_image,
                                                 const vector<vector<unsigned char>> &source_color_image,
                                                 const vector<unsigned short> &last_object_depth_image,
                                                 const vector<vector<unsigned char>> &last_object_color_image,
                                                 vector<unsigned short> *composed_depth_image,
                                                 vector<vector<unsigned char>> *composed_color_image) {

  if (cost_debug_msgs)
    printf("GetComposedDepthImage() with color and depth\n");
  // printf("source_depth_image : %f\n", source_depth_image.size());

  composed_depth_image->clear();
  composed_depth_image->resize(source_depth_image.size(), kKinectMaxDepth);

  // vector<unsigned char> color_vector{'0','0','0'};
  composed_color_image->clear();
  // composed_color_image->resize(source_color_image.size(), color_vector);
  composed_color_image->resize(source_color_image.size());

  // printf("composed_color_image : %f\n", composed_color_image->size());
  assert(source_depth_image.size() == last_object_depth_image.size());

  #pragma omp parallel for

  for (size_t ii = 0; ii < source_depth_image.size(); ++ii) {
    composed_depth_image->at(ii) = std::min(source_depth_image[ii],
                                            last_object_depth_image[ii]);

    if (env_params_.use_external_render == 1 || perch_params_.use_color_cost)
    {
      if (source_depth_image[ii] <= last_object_depth_image[ii])
      {
          composed_color_image->at(ii) = source_color_image[ii];
      }
      else
      {
          composed_color_image->at(ii) = last_object_color_image[ii];
      }
    }
  }
  if (cost_debug_msgs)
    printf("GetComposedDepthImage() Done\n");
  return true;
}

bool EnvObjectRecognition::GetComposedDepthImage(const vector<unsigned short>
                                                 &source_depth_image, const vector<unsigned short> &last_object_depth_image,
                                                 vector<unsigned short> *composed_depth_image) {

  composed_depth_image->clear();
  composed_depth_image->resize(source_depth_image.size(), kKinectMaxDepth);
  assert(source_depth_image.size() == last_object_depth_image.size());

  #pragma omp parallel for

  for (size_t ii = 0; ii < source_depth_image.size(); ++ii) {
    composed_depth_image->at(ii) = std::min(source_depth_image[ii],
                                            last_object_depth_image[ii]);
  }

  return true;
}

bool EnvObjectRecognition::GetSingleObjectDepthImage(const GraphState
                                                     &single_object_graph_state, vector<unsigned short> *single_object_depth_image,
                                                     bool after_refinement) {

  single_object_depth_image->clear();

  assert(single_object_graph_state.NumObjects() == 1);

  auto &cache = after_refinement ? adjusted_single_object_depth_image_cache_ :
                unadjusted_single_object_depth_image_cache_;

  // TODO: Verify there are no cases where this will fail.
  if (cache.find(single_object_graph_state) ==
      cache.end()) {
    return false;
  }

  *single_object_depth_image =
    cache[single_object_graph_state];

  return true;
}

bool EnvObjectRecognition::GetSingleObjectHistogramScore(const GraphState
                                                     &single_object_graph_state, double &histogram_score) {
  assert(single_object_graph_state.NumObjects() == 1);

  // TODO: Verify there are no cases where this will fail.
  if (adjusted_single_object_histogram_score_cache_.find(single_object_graph_state) ==
      adjusted_single_object_histogram_score_cache_.end()) {
    return false;
  }

  histogram_score =
    adjusted_single_object_histogram_score_cache_[single_object_graph_state];

  return true;
}

vector<unsigned short> EnvObjectRecognition::ApplyOcclusionMask(
  const vector<unsigned short> input_depth_image,
  const vector<unsigned short> masking_depth_image) {
  vector<unsigned short> masked_depth_image(kNumPixels, kKinectMaxDepth);

  for (int ii = 0; ii < kNumPixels; ++ii) {
    masked_depth_image[ii] = masking_depth_image[ii] > input_depth_image[ii] ?
                             input_depth_image[ii] : kKinectMaxDepth;
  }

  return masked_depth_image;
}
}  // namespace
