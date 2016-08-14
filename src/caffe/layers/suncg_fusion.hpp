//
// Created by Fisher Yu on 8/13/16.
//

#ifndef CAFFE_SUNCG_FUSION_HPP_HPP
#define CAFFE_SUNCG_FUSION_HPP_HPP

#include <vector>

#include "opencv2/imgcodecs.hpp"

#define CUDA_NUM_THREADS 512

#define MAX_NUM_BLOCKS 2880

#define GPUCompute2Dtype(x) (x)
#define GPUStorage2Dtype(x) (x)

inline int CUDA_GET_BLOCKS(const size_t N) {
  return min(MAX_NUM_BLOCKS, int((N + size_t(CUDA_NUM_THREADS) - 1) / CUDA_NUM_THREADS));
}

inline size_t CUDA_GET_LOOPS(const size_t N) {
  size_t total_threads = CUDA_GET_BLOCKS(N)*CUDA_NUM_THREADS;
  return (N + total_threads -1)/ total_threads;
}

template <typename Dtype>
__global__ void Kernel_set_value(size_t CUDA_NUM_LOOPS, size_t N, Dtype* GPUdst, Dtype value){
  const size_t idxBase = size_t(CUDA_NUM_LOOPS) * (size_t(CUDA_NUM_THREADS) * size_t(blockIdx.x) + size_t(threadIdx.x));
  if (idxBase >= N) return;
  for (size_t idx = idxBase; idx < min(N,idxBase+CUDA_NUM_LOOPS); ++idx ){
    GPUdst[idx] = value;
  }
}


template <typename Dtype>
void ReadDepthImage(const std::string &filename, Dtype * depth_data,
                    int frame_width, int frame_height) {
  cv::Mat depth_image = cv::imread(filename.c_str(), 0);
  unsigned short * depth_raw = new unsigned short[frame_height * frame_width];
  for (int i = 0; i < frame_height * frame_width; ++i) {
    depth_raw[i] = ((((unsigned short)depth_image.data[i * 2 + 1]) << 8) +
        ((unsigned short)depth_image.data[i * 2 + 0]));
    depth_raw[i] = (depth_raw[i] << 13 | depth_raw[i] >> 3);
    depth_data[i] = Dtype((float)depth_raw[i] / 1000.0f);
  }
  delete [] depth_raw;
}

template <typename Dtype>
void ReadVoxLabel(const std::string &filename, Dtype *vox_origin,
                  Dtype *cam_pose, Dtype *occupancy_label_fullsize,
                  std::vector<int> segmentation_class_map,
                  Dtype *segmentation_label_fullsize) {

  // Open file
  std::ifstream fid(filename, std::ios::binary);

  // Read voxel origin in world coordinates
  for (int i = 0; i < 3; ++i)
    fid.read((char*)&vox_origin[i], sizeof(Dtype));

  // Read camera pose
  for (int i = 0; i < 16; ++i)
    fid.read((char*)&cam_pose[i], sizeof(Dtype));

  // Read voxel label data from file (RLE compression)
  std::vector<unsigned int> scene_vox_RLE;
  while (!fid.eof()) {
    int tmp;
    fid.read((char*)&tmp, sizeof(int));
    if (!fid.eof())
      scene_vox_RLE.push_back(tmp);
  }

  // Reconstruct voxel label volume from RLE
  int vox_idx = 0;
  for (size_t i = 0; i < scene_vox_RLE.size() / 2; ++i) {
    unsigned int vox_val = scene_vox_RLE[i * 2];
    unsigned int vox_iter = scene_vox_RLE[i * 2 + 1];
    for (size_t j = 0; j < vox_iter; ++j) {
      segmentation_label_fullsize[vox_idx] = Dtype(segmentation_class_map[vox_val]);
      if (vox_val > 0)
        occupancy_label_fullsize[vox_idx] = Dtype(1.0f);
      else
        occupancy_label_fullsize[vox_idx] = Dtype(0.0f);
      vox_idx++;
    }
  }
}

template <typename Dtype>
void GPU_set_value(size_t N, Dtype* GPUdst, Dtype value){
  Kernel_set_value<<<CUDA_GET_BLOCKS(N), CUDA_NUM_THREADS>>>(CUDA_GET_LOOPS(N),N,GPUdst,value);
  CUDA_CHECK(cudaGetLastError());
}

template <typename Dtype>
void GPU_set_zeros(size_t N, Dtype* GPUdst) {
  GPU_set_value(N, GPUdst, Dtype(0));
}

/*-------------------- Fusion Utilities ---------------------*/
// Integrate voxel volume

template <typename Dtype>
__global__ void Integrate(Dtype * cam_info, Dtype * vox_info, Dtype * depth_data, Dtype * vox_tsdf, Dtype * vox_weight, Dtype * vox_height) {

  // Get camera information
  int frame_width = cam_info[0];
  int frame_height = cam_info[1];
  Dtype cam_K[9];
  for (int i = 0; i < 9; ++i)
    cam_K[i] = cam_info[i + 2];
  Dtype cam_pose[16];
  for (int i = 0; i < 16; ++i)
    cam_pose[i] = cam_info[i + 11];

  // Get voxel volume parameters
  Dtype vox_unit = vox_info[0];
  Dtype vox_margin = vox_info[1];
  int vox_size[3];
  for (int i = 0; i < 3; ++i)
    vox_size[i] = vox_info[i + 2];
  Dtype vox_origin[3];
  for (int i = 0; i < 3; ++i)
    vox_origin[i] = vox_info[i + 5];

  int z = blockIdx.x;
  int y = threadIdx.x;
  for (int x = 0; x < vox_size[0]; ++x) {
    int vox_idx = z * vox_size[0] * vox_size[1] + y * vox_size[0] + x;

    // Get point in world coordinates XYZ -> YZX
    Dtype point_base[3] = {0};
    point_base[0] = Dtype(z) * vox_unit + vox_origin[0];
    point_base[1] = Dtype(x) * vox_unit + vox_origin[1];
    point_base[2] = Dtype(y) * vox_unit + vox_origin[2];

    // Encode height from floor
    if (vox_height != NULL) {
      Dtype height_val = ((point_base[2] + 0.2f) / 2.5f);
      vox_height[vox_idx] = GPUCompute2Dtype(fmin(1.0f, fmax(height_val, 0.0f)));
    }

    // Get point in current camera coordinates
    Dtype point_cam[3] = {0};
    point_base[0] = point_base[0] - cam_pose[0 * 4 + 3];
    point_base[1] = point_base[1] - cam_pose[1 * 4 + 3];
    point_base[2] = point_base[2] - cam_pose[2 * 4 + 3];
    point_cam[0] = cam_pose[0 * 4 + 0] * point_base[0] + cam_pose[1 * 4 + 0] * point_base[1] + cam_pose[2 * 4 + 0] * point_base[2];
    point_cam[1] = cam_pose[0 * 4 + 1] * point_base[0] + cam_pose[1 * 4 + 1] * point_base[1] + cam_pose[2 * 4 + 1] * point_base[2];
    point_cam[2] = cam_pose[0 * 4 + 2] * point_base[0] + cam_pose[1 * 4 + 2] * point_base[1] + cam_pose[2 * 4 + 2] * point_base[2];
    if (point_cam[2] <= 0)
      continue;

    // Project point to 2D
    int pixel_x = roundf(cam_K[0] * (point_cam[0] / point_cam[2]) + cam_K[2]);
    int pixel_y = roundf(cam_K[4] * (point_cam[1] / point_cam[2]) + cam_K[5]);
    if (pixel_x < 0 || pixel_x >= frame_width || pixel_y < 0 || pixel_y >= frame_height){ // outside FOV
      //vox_tsdf[vox_idx] = GPUCompute2Dtype(-1.0);
      continue;
    }


    // Get depth
    Dtype point_depth = depth_data[pixel_y * frame_width + pixel_x];
    if (point_depth < Dtype(0.0f) || point_depth > Dtype(10.0f))
      continue;
    if (roundf(point_depth) == 0){ // mising depth
      vox_tsdf[vox_idx] = GPUCompute2Dtype(-1.0);
      continue;
    }


    // Get depth difference
    Dtype point_dist = (point_depth - point_cam[2]) * sqrtf(1 + powf((point_cam[0] / point_cam[2]), 2) + powf((point_cam[1] / point_cam[2]), 2));

    // Integrate
    if (point_dist > -vox_margin) {
      Dtype sdf = fmin(Dtype(1.0f), point_dist / vox_margin);
      Dtype weight_old = vox_weight[vox_idx];
      Dtype weight_new = weight_old + Dtype(1.0f);
      vox_weight[vox_idx] = weight_new;
      vox_tsdf[vox_idx] = GPUCompute2Dtype((GPUStorage2Dtype(vox_tsdf[vox_idx]) * weight_old + sdf) / weight_new);
    }else{
      vox_tsdf[vox_idx] = GPUCompute2Dtype(-1.0);
    }
  }
}

#endif //CAFFE_SUNCG_FUSION_HPP_HPP
