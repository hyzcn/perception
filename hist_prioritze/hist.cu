#include "./include/hist_prioritize/hist.h"
// #include <math.h> 
#include "cuda_fp16.h"

// #include <numeric> 
#define SQR(x) ((x)*(x))
#define POW2(x) SQR(x)
#define POW3(x) ((x)*(x)*(x))
#define POW4(x) (POW2(x)*POW2(x))
#define POW7(x) (POW3(x)*POW3(x)*(x))
#define DegToRad(x) ((x)*M_PI/180)
#define RadToDeg(x) ((x)/M_PI*180)

namespace hist_prioritize {

template<typename T>
device_vector_holder<T>::~device_vector_holder(){
    __free();
}

template<typename T>
void device_vector_holder<T>::__free(){
    if(valid){
        cudaFree(__gpu_memory);
        valid = false;
        __size = 0;
    }
}

template<typename T>
device_vector_holder<T>::device_vector_holder(size_t size_, T init)
{
    __malloc(size_);
    thrust::fill(begin_thr(), end_thr(), init);
}

template<typename T>
void device_vector_holder<T>::__malloc(size_t size_){
    if(valid) __free();
    cudaMalloc((void**)&__gpu_memory, size_ * sizeof(T));
    __size = size_;
    valid = true;
}

template<typename T>
device_vector_holder<T>::device_vector_holder(size_t size_){
    __malloc(size_);
}

template class device_vector_holder<int>;

void print_cuda_memory_usage(){
    // show memory usage of GPU

    size_t free_byte ;
    size_t total_byte ;
    auto cuda_status = cudaMemGetInfo( &free_byte, &total_byte ) ;

    if ( cudaSuccess != cuda_status ){
        printf("Error: cudaMemGetInfo fails, %s \n", cudaGetErrorString(cuda_status) );
        exit(1);
    }

    double free_db = (double)free_byte ;
    double total_db = (double)total_byte ;
    double used_db = total_db - free_db ;
    printf("GPU memory usage: used = %f, free = %f MB, total = %f MB\n",
        used_db/1024.0/1024.0, free_db/1024.0/1024.0, total_db/1024.0/1024.0);
}

struct max2zero_functor{

    max2zero_functor(){}

    __host__ __device__
    int32_t operator()(const int32_t& x) const
    {
      return (x==INT_MAX)? 0: x;
    }
};

struct pose_functor{

    pose_functor(){}

    __host__ __device__
    pose operator()(const pose& x) const
    {
      return x;
    }
};

__global__ void compare_hist(const int ob_pixel_num, const int num,const pose* output_p, 
                             const int*hist, int* output_s,const int y_num, const int angle_num){
    size_t index = blockIdx.x*blockDim.x + threadIdx.x;
    if(index<num){

        float x = output_p[index].x;
        float y = output_p[index].y;
        float theta = output_p[index].theta;
        int con_y_num = 140;
        int con_angle_num = 126;
        int trans_i = int((x-(-0.44))/0.01*con_y_num*con_angle_num+(y-(-0.7))/0.01*con_angle_num);
        int angle_i = int((theta)/0.05+0.5f);
        int ind = trans_i+angle_i+1;

        int score = ob_pixel_num;
        int bar_num = 13;
        int total_rendered = 0;
        for(int i =0; i <bar_num-1;i++){
            int ob = output_p[index].hist[i];
            int re = hist[ind*bar_num+i];
            total_rendered += re;
            score -=ob;
            int diff = ob-re;
            if(diff>0){
                score += diff/2;
                score -= re;
            }else{
                score -= ob;
                score -= diff/2;
            }
        }
        int ob_b = output_p[index].hist[12];
        int re_b = hist[ind*bar_num+12];
        int db = ob_b-re_b;
        if(db>0){score+=db/2;}else{score-=db/2;}
        // int ob_h1 = output_p[index].hist[0];
        // int ob_h2 = output_p[index].hist[1];
        // int ob_h3 = output_p[index].hist[2];
        // int ob_h4 = output_p[index].hist[3];
        // int ob_h5 = output_p[index].hist[4];
        // int ob_h6 = output_p[index].hist[5];
        // int ob_h7 = output_p[index].hist[6];
        // int re_h1 = hist[ind*13];
        // int re_h2 = hist[ind*13+1];
        // int re_h3 = hist[ind*13+2];
        // int re_h4 = hist[ind*7+3];
        // int re_h5 = hist[ind*7+4];
        // int re_h6 = hist[ind*7+5];
        // int re_h7 = hist[ind*7+6];
        // score =score-ob_h1-ob_h2-ob_h3-ob_h4-ob_h5-ob_h6;
        // int d0 = ob_h1-re_h1;
        // int d1 = ob_h2-re_h2;
        // int d2 = ob_h3-re_h3;
        // int d3 = ob_h4-re_h4;
        // int d4 = ob_h5-re_h5;
        // int d5 = ob_h6-re_h6;
        // int d6 = ob_h7-re_h7;
        // if(d0>0){score+=d0/2;score-=re_h1;}else{score-=d0/2;score-=ob_h1;}
        // if(d1>0){score+=d1/2;score-=re_h2;}else{score-=d1/2;score-=ob_h2;}
        // if(d2>0){score+=d2/2;score-=re_h3;}else{score-=d2/2;score-=ob_h3;}
        // if(d3>0){score+=d3/2;score-=re_h4;}else{score-=d3/2;score-=ob_h4;}
        // if(d4>0){score+=d4/2;score-=re_h5;}else{score-=d4/2;score-=ob_h5;}
        // if(d5>0){score+=d5/2;score-=re_h6;}else{score-=d5/2;score-=ob_h6;}
        // if(d6>0){score+=d6/2;}else{score-=d6/2;}
        output_s[index] = score+518400;

        // printf("%d: %f,%f,%f: %d: %d %d %d\n",index,output_p[index].x,output_p[index].y,
        //                                                output_p[index].theta,ind,hist[ind*bar_num],hist[ind*bar_num+1],hist[ind*bar_num+2]
        //                                                );
    }


}


// bool inside_ROI(x_min,x_max,y_min,y_max){
//     return x_min>431 && y_min > 246 && x_max < 431+136 &&y_max< 246+94;
// }
__device__ bool inside_ROI(float x_min,float x_max,float y_min,float y_max, const int* roi){
    int count = roi[0];
    for(int i =0; i < count; i ++){
        int x = roi[i*4+1];
        int y = roi[i*4+2];
        int width = roi[i*4+3];
        int height = roi[i*4+4];
        if(x_min>(x-10) && y_min > (y-10) && x_max < (x+width+10) &&y_max< (y+height+10))
            return true;
    }
    return false;
}




__global__ void construct_hist(int32_t* out_score,int32_t* valid_p,pose* output_p,
                                   const int width, const int height,
                                   const float x_min, const float y_min, const float angle_min,const float x_max, const float y_max, const float angle_max,
                                   const float trans_res,const float angle_res,const int x_num,const int y_num,const int angle_num,
                                   const uint8_t* h_ob, const uint8_t* s_ob,const uint8_t* v_ob,
                                   const float* cam_r1,const float* cam_r2,const float* cam_r3,
                                   const float* bb, const int* hist, const int* roi)
{
    size_t angle_i = blockIdx.y;
    size_t trans_i = blockIdx.x*blockDim.x + threadIdx.x;
    int32_t output_index = (angle_num)*trans_i+angle_i;
    float x = x_min+(int)floorf(trans_i/y_num)*trans_res;
    float y = y_min+(trans_i%y_num)*trans_res;
    float theta = angle_min+angle_i*angle_res;
    if(output_index<x_num*y_num*angle_num){
        float min_x=10000;
        float max_x=-10000;
        float min_y=10000;
        float max_y=-10000;
        // printf("%d,%d: %f,%f,%f;\n",output_index,x_num*y_num*angle_num,x,y,theta);
        for(int i =0; i <8;i++){
            float cur_x = bb[i*3];
            float cur_y = bb[i*3+1];
            float cur_z = bb[i*3+2];

            float res_x = cur_x*(cam_r1[0]*cos(theta)+cam_r1[1]*(sin(theta)))+
                          cur_y*(cam_r1[0]*(-sin(theta))+cam_r1[1]*(cos(theta)))+
                          cur_z*cam_r1[2]+
                          //!!!!!!!!!!!!!!!!!!!!!!!!!!!0.0480247 is from the center of object to the z value find the same number in search_env.cpp 
                          cam_r1[0]*x+cam_r1[1]*y+cam_r1[2]*0.0480247+cam_r1[3];
            float res_y = cur_x*(cam_r2[0]*cos(theta)+cam_r2[1]*(sin(theta)))+
                          cur_y*(cam_r2[0]*(-sin(theta))+cam_r2[1]*(cos(theta)))+
                          cur_z*cam_r2[2]+
                          cam_r2[0]*x+cam_r2[1]*y+cam_r2[2]*0.0480247+cam_r2[3];
            float res_z = cur_x*(cam_r3[0]*cos(theta)+cam_r3[1]*(sin(theta)))+
                          cur_y*(cam_r3[0]*(-sin(theta))+cam_r3[1]*(cos(theta)))+
                          cur_z*cam_r3[2]+
                          cam_r3[0]*x+cam_r3[1]*y+cam_r3[2]*0.0480247+cam_r3[3];
            float bx = res_x/res_z;
            float by = res_y/res_z;
            if(bx<min_x) min_x = bx;
            if(bx>max_x) max_x = bx;
            if(by<min_y) min_y = by;
            if(by>max_y) max_y = by;
            // if(theta==1){
            //     printf("%f,%f,%f,%f;\n",cam_r1[0]*cos(theta)+cam_r1[1]*(sin(theta)),
            //         cam_r1[0]*(-sin(theta))+cam_r1[1]*(cos(theta)),
            //         cam_r1[2],cam_r1[0]*x+cam_r1[1]*y+cam_r1[2]*0.0480247+cam_r1[3]);
            //     // printf("%f,%f,%d;\n",res_x,res_y,output_index);
            // }
            
            // printf("%f,%f,%f,%f,%d;\n",cam_r1[0],cam_r1[1],cam_r1[2],cam_r1[3],output_index);
        }
        // printf("%d, !!!!%f,%f,%f:   %f,%f,%f,%f;\n",output_index,x,y,theta,min_x,max_x,min_y,max_y);
        int sum = 0;
        if(min_x>=0 && min_x<width&&max_x>=0 && max_x<width&&
            min_y>=0 && min_y<height&&max_y>=0 && max_y<height&&inside_ROI(min_x,max_x,min_y,max_y,roi)){
            // min_y>=0 && min_y<height&&max_y>=0 && max_y<height){
            for(int cur_x = min_x;cur_x<=max_x;cur_x++){
                for(int cur_y=min_y;cur_y<=max_y;cur_y++){
                    int cur_ind = cur_y*width+cur_x;
                    uint8_t h_value = h_ob[cur_ind];
                    uint8_t s_value = s_ob[cur_ind];
                    uint8_t v_value = v_ob[cur_ind];
                    if(s_value!=0 && v_value!=0){
                        sum+=1;
                        int index = h_value/15;
                        output_p[output_index].hist[index] +=1;
                    }else{
                        output_p[output_index].hist[12] +=1;;
                    }
                }
            }
            if(sum <1){
                out_score[output_index] = 2*518400;
                // int32_t& valid_add = valid_p[0];
                // atomicAdd(&valid_add,1);
                // out_score[output_index] = 0;
            }else{
                // printf("%f,%f,%f,%d;\n",x,y,theta,sum);
                // printf("%f,%f,%f,%f;\n",min_x,min_y,max_x,max_y);
                int32_t& valid_add = valid_p[0];
                atomicAdd(&valid_add,1);
                out_score[output_index] = 0;
                output_p[output_index].x = x;
                output_p[output_index].y = y;
                output_p[output_index].theta = theta;
            }
            

        }else{
            out_score[output_index] =2*518400;

        }
        
    }
    
}





s_pose compare_hist(const int width, const int height,const float x_min,const float x_max,
                              const float y_min,const float y_max,
                              const float theta_min,const float theta_max,
                              const float trans_res, const float angle_res,
                              const int32_t ob_pixel_num,
                              const std::vector<std::vector<uint8_t>>& observed,
                              const std::vector<std::vector<float> >& cam_matrix,
                              const std::vector<float>& bounding_boxes,
                              const std::vector<int>& hist_vector,
                              const std::vector<int>& color_region
                              )
{

    float elapsed1=0;
    float elapsed2=0;
    cudaEvent_t start1, stop1,start2,stop2;

    HANDLE_ERROR(cudaEventCreate(&start1));
    HANDLE_ERROR(cudaEventCreate(&stop1));

    HANDLE_ERROR( cudaEventRecord(start1, 0));

    const size_t threadsPerBlock = 256;
    
    float x_range = x_max-x_min;
    float y_range = y_max-y_min;
    float angle_range = theta_max-theta_min;
    int x_num =(int)floor(x_range / trans_res* 10+0.5)/10+1;
    int y_num =(int)floor(y_range / trans_res*10+0.5)/10+1;
    int angle_num = (int)floor(angle_range/angle_res*10+0.5)/10+1;

    // std::cout<<x_range<<","<<y_range<<","<<angle_range<<std::endl;
    // std::cout<<x_num<<","<<y_num<<","<<angle_num<<std::endl;
    // for(int i =0; i <hist_vector.size(); i ++){
    //     std::cout<<hist_vector[i]<<",";
    // }
    thrust::device_vector<pose> d_output_p(x_num*y_num*angle_num);
    thrust::device_vector<int> output_score(x_num*y_num*angle_num, 0);
    thrust::device_vector<uint8_t> d_h_ob = observed[0];
    thrust::device_vector<uint8_t> d_s_ob = observed[1];
    thrust::device_vector<uint8_t> d_v_ob = observed[2];
    thrust::device_vector<float> cam_row1 = cam_matrix[0];
    thrust::device_vector<float> cam_row2 = cam_matrix[1];
    thrust::device_vector<float> cam_row3 = cam_matrix[2];
    thrust::device_vector<int> h = hist_vector;
    thrust::device_vector<int> c_region = color_region;
    thrust::device_vector<int> d_valid(1, 0);
    // std::cout<<cam_matrix[0][0]<<","<<cam_matrix[0][1]<<","<<cam_matrix[0][2]<<","<<cam_matrix[0][3]<<std::endl;
    // std::cout<<cam_matrix[1][0]<<","<<cam_matrix[1][1]<<","<<cam_matrix[1][2]<<","<<cam_matrix[1][3]<<std::endl;
    // std::cout<<cam_matrix[2][0]<<","<<cam_matrix[2][1]<<","<<cam_matrix[2][2]<<","<<cam_matrix[2][3]<<std::endl;
    thrust::device_vector<float> bb = bounding_boxes;

    {

        int32_t* output_s = thrust::raw_pointer_cast(output_score.data());
        pose* output_p = thrust::raw_pointer_cast(d_output_p.data());
        int32_t* d_valid_p = thrust::raw_pointer_cast(d_valid.data());
        uint8_t* h_ob = thrust::raw_pointer_cast(d_h_ob.data());
        uint8_t* s_ob = thrust::raw_pointer_cast(d_s_ob.data());
        uint8_t* v_ob = thrust::raw_pointer_cast(d_v_ob.data());
        float* cam_r1 = thrust::raw_pointer_cast(cam_row1.data());
        float* cam_r2 = thrust::raw_pointer_cast(cam_row2.data());
        float* cam_r3 = thrust::raw_pointer_cast(cam_row3.data());
        float* bounding_box = thrust::raw_pointer_cast(bb.data());
        int* hist = thrust::raw_pointer_cast(h.data());
        int* roi = thrust::raw_pointer_cast(c_region.data());
        // glm::mat4* a = thrust::raw_pointer_cast(cam_matrix.data());


        dim3 numBlocks((x_num*y_num + threadsPerBlock - 1) / threadsPerBlock, angle_num);
        construct_hist<<<numBlocks, threadsPerBlock>>>(output_s,d_valid_p,output_p,
                                                        width,height,
                                                        x_min, y_min, theta_min,x_max, y_max, theta_max,
                                                        trans_res,angle_res,x_num,y_num,angle_num,
                                                        h_ob,s_ob,v_ob,
                                                        cam_r1,cam_r2,cam_r3,bounding_box,hist,roi);
        cudaDeviceSynchronize();
    }
    

    std::vector<int> score(x_num*y_num*angle_num);
    std::vector<int> x(x_num*y_num*angle_num);
    std::vector<int> y(x_num*y_num*angle_num);
    std::vector<int> theta(x_num*y_num*angle_num);
    
    std::vector<int> valid_num(1);
    {
        // thrust::transform(output_score.begin(), output_score.end(),
        //                   output_score.begin(), max2zero_functor());
        // thrust::copy(output_score.begin(), output_score.end(), score.begin());

        thrust::transform(d_valid.begin(), d_valid.end(),
                          d_valid.begin(), max2zero_functor());
        thrust::copy(d_valid.begin(), d_valid.end(), valid_num.begin());
        
        thrust::sort_by_key(output_score.begin(), output_score.end(), d_output_p.begin());
        thrust::transform(d_output_p.begin(), d_output_p.end(),
                          d_output_p.begin(),pose_functor());
        


    }

    HANDLE_ERROR(cudaEventRecord(stop1, 0));
    HANDLE_ERROR(cudaEventSynchronize (stop1) );

    HANDLE_ERROR(cudaEventElapsedTime(&elapsed1, start1, stop1) );

    HANDLE_ERROR(cudaEventDestroy(start1));
    HANDLE_ERROR(cudaEventDestroy(stop1));

    printf("The constructing hist time was %.2f ms\n", elapsed1);

    HANDLE_ERROR(cudaEventCreate(&start2));
    HANDLE_ERROR(cudaEventCreate(&stop2));

    HANDLE_ERROR( cudaEventRecord(start2, 0));

    
    {

        int32_t* output_s = thrust::raw_pointer_cast(output_score.data());
        pose* output_p = thrust::raw_pointer_cast(d_output_p.data());
        int* hist = thrust::raw_pointer_cast(h.data());

        dim3 numBlocks((valid_num[0] + threadsPerBlock - 1) / threadsPerBlock, 1);
        compare_hist<<<numBlocks, threadsPerBlock>>>(ob_pixel_num,valid_num[0],output_p,hist,output_s,y_num,angle_num);
        cudaDeviceSynchronize();
    }
    {   
        
        
        
        thrust::sort_by_key(output_score.begin(), output_score.end(), d_output_p.begin());
        thrust::transform(d_output_p.begin(), d_output_p.end(),
                          d_output_p.begin(),pose_functor());

        thrust::sort_by_key(output_score.begin(), output_score.end(), output_score.begin());
        thrust::transform(output_score.begin(), output_score.end(),
                          output_score.begin(), max2zero_functor());
        thrust::copy(output_score.begin(), output_score.begin()+valid_num[0], score.begin());
        


    }
    HANDLE_ERROR(cudaEventRecord(stop2, 0));
    HANDLE_ERROR(cudaEventSynchronize (stop2) );

    HANDLE_ERROR(cudaEventElapsedTime(&elapsed2, start2, stop2) );

    HANDLE_ERROR(cudaEventDestroy(start2));
    HANDLE_ERROR(cudaEventDestroy(stop2));

    printf("The comparing hist time was %.2f ms\n", elapsed2);
    std::vector<pose> v(valid_num[0]);
    thrust::copy(d_output_p.begin(), d_output_p.begin()+valid_num[0], v.begin());
    std::cout<<"aaaaaaa"<<valid_num[0]<<std::endl;
    std::vector<std::vector<int> > res;
    s_pose a;
    a.ps = v;
    a.score = score;
    // for(int i =0; i <valid_num[0]; i ++){
    //     pose a = v[i];
    //     std::cout<<a.x<<","<<a.y<<","<<a.theta<<":"<<score[i]<<std::endl;
    // }
    // for(int i =0; i <score.size(); i ++){
    //     std::cout<<score[i]<<",";
    // }
    // res.push_back(min_x);
    // res.push_back(max_x);
    // res.push_back(min_y);
    // res.push_back(max_y);
    // std::cout<<min_y.size()<<"jijijij";
    // for(int i =0; i <min_x.size(); i++){
    //     std::cout<<min_x[i]<<","<<max_x[i]<<","<<min_y[i]<<","<<max_y[i]<<std::endl;
    // }

    return a;
}



}