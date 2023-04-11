// Copyright (c) 2020- Margarita Grinvald, Autonomous Systems Lab, ETH Zurich
// Licensed under the MIT License (see LICENSE for details)

#include "tsdf_plusplus/visualizer/visualizer.h"

#include <pcl/surface/vtk_smoothing/vtk_utils.h>

#include "tsdf_plusplus/utils/conversions.h"
#include "tsdf_plusplus/visualizer/visualizer_utils.h"
#include <tsdf_plusplus/utils/file_utils.h>

Visualizer::Visualizer(std::shared_ptr<voxblox::MeshLayer> mesh_layer,
                       std::shared_ptr<std::mutex> mesh_layer_mutex,
                       std::shared_ptr<bool> mesh_layer_updated,
                       std::shared_ptr<Eigen::Matrix4f> camera_extrinsics,
                       const Eigen::Matrix3f &camera_intrinsics,
                       const std::string export_path,
                       bool frames_as_mask_images)
    : mesh_layer_(mesh_layer.get()), mesh_layer_mutex_(mesh_layer_mutex.get()),
      mesh_layer_updated_(mesh_layer_updated.get()),
      camera_extrinsics_(camera_extrinsics.get()),
      camera_intrinsics_(camera_intrinsics), export_path_(export_path),
      save_screenshot_(false) {}

void Visualizer::run()
{
}

void Visualizer::triggerScreenshot(uint32_t frame_number)
{
}

void Visualizer::saveScreenshot()
{
}
