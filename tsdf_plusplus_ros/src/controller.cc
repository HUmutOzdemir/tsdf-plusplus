// Copyright (c) 2020- Margarita Grinvald, Autonomous Systems Lab, ETH Zurich
// Licensed under the MIT License (see LICENSE for details)

#include "tsdf_plusplus_ros/controller.h"

#include <fstream>
#include <iostream>
#include <unordered_map>

#include <minkindr_conversions/kindr_tf.h>
#include <pcl/console/time.h>
#include <pcl_conversions/pcl_conversions.h>
#include <tsdf_plusplus/alignment/icp_utils.h>
#include <tsdf_plusplus/core/common.h>
#include <tsdf_plusplus/core/map.h>
#include <tsdf_plusplus/integrator/integrator.h>
#include <tsdf_plusplus/mesh/mesh_integrator.h>
#include <tsdf_plusplus/utils/conversions.h>
#include <tsdf_plusplus/utils/file_utils.h>
#include <tsdf_plusplus_msgs/MovementPointCloud.h>
#include <tsdf_plusplus_msgs/SegmentedPointCloud.h>
#include <tsdf_plusplus_msgs/ObjectMapInformation.h>
#include <voxblox/io/mesh_ply.h>
#include <voxblox/io/sdf_ply.h>
#include <voxblox_msgs/Mesh.h>
#include <voxblox_ros/conversions.h>
#include <voxblox_ros/mesh_vis.h>

#include "tsdf_plusplus_ros/ros_params.h"

Controller::Controller(const ros::NodeHandle &nh,
                       const ros::NodeHandle &nh_private)
    : Controller(nh, nh_private, getMapConfigFromRosParam(nh_private),
                 getIntegratorConfigFromRosParam(nh_private),
                 getICPConfigFromRosParam(nh_private),
                 getMeshIntegratorConfigFromRosParam(nh_private)) {}

Controller::Controller(const ros::NodeHandle &nh,
                       const ros::NodeHandle &nh_private,
                       const Map::Config &map_config,
                       const Integrator::Config &integrator_config,
                       const ICP::Config &icp_config,
                       const MOMeshIntegrator::Config &mesh_config)
    : nh_(nh), nh_private_(nh_private), frame_number_(0u),
      world_frame_("world"), sensor_frame_(""),
      using_ground_truth_segmentation_(false), object_tracking_enabled_(false),
      ground_truth_tracking_(false) {
  getConfigFromRosParam(nh_private);

  last_segment_msg_time_ = ros::Time(0);

  // Subcribe to input pointcloud.
  std::string segment_pointcloud_topic =
      "/depth_segmentation_node/object_segment";
  nh_private_.param<std::string>("segment_pointcloud_topic",
                                 segment_pointcloud_topic,
                                 segment_pointcloud_topic);

  // TODO(margaritaG): careful with how this is handled.
  int pointcloud_queue_size = 1000;
  nh_private_.param("pointcloud_queue_size", pointcloud_queue_size,
                    pointcloud_queue_size);
  pointcloud_sub_ =
      nh_.subscribe(segment_pointcloud_topic, pointcloud_queue_size,
                    &Controller::segmentPointcloudCallback, this);

  std::string reset_topic = "/tsdf_plusplus_node/reset";
  nh_private_.param<std::string>("reset_topic", reset_topic, reset_topic);
  reset_sub_ = nh_.subscribe(reset_topic, 1, &Controller::resetCallback, this);

  // Initialize map and integrator.
  map_.reset(new Map(map_config));
  integrator_.reset(new Integrator(integrator_config, map_));

  icp_.reset(new ICP(icp_config));

  // Initialize mesh and mesh integrator.
  mesh_layer_.reset(new MeshLayer(map_->block_size()));
  mesh_integrator_.reset(new MOMeshIntegrator(mesh_config, map_, mesh_layer_));
  mesh_layer_updated_.reset(new bool(false));
  mesh_layer_mutex_.reset(new std::mutex);

  // If set, use a timer to progressively integrate the mesh.
  double update_mesh_every_n_sec = 1.0;
  nh_private_.param("meshing/update_mesh_every_n_sec", update_mesh_every_n_sec,
                    update_mesh_every_n_sec);

  if (update_mesh_every_n_sec > 0.0) {
    update_mesh_timer_ =
        nh_private_.createTimer(ros::Duration(update_mesh_every_n_sec),
                                &Controller::updateMeshEvent, this);
  }

  bool enable_visualizer = false;
  nh_private_.param("visualizer/enable", enable_visualizer, enable_visualizer);

  camera_extrinsics_.reset(new Eigen::Matrix4f());

  // Initialize visualizer.
  if (enable_visualizer) {
    visualizer_.reset(new Visualizer(
        mesh_layer_, mesh_layer_mutex_, mesh_layer_updated_, camera_extrinsics_,
        camera_intrinsics_, export_path_, write_frames_to_file_));
    vizualizer_thread_ = std::thread(&Visualizer::run, visualizer_.get());
  } else {
    // If visualizer disabled, its frames cannot be written to files.
    write_frames_to_file_ = false;
  }

  // Advertise services.
  generate_mesh_srv_ = nh_private_.advertiseService(
      "generate_mesh", &Controller::generateMeshCallback, this);
  save_objects_srv_ = nh_private_.advertiseService(
      "save_objects", &Controller::saveObjectsCallback, this);
  remove_objects_srv_ = nh_private_.advertiseService(
      "remove_objects", &Controller::removeObjectsCallback, this);

  // Advertise publishers.
  mesh_pub_ = nh_private_.advertise<voxblox_msgs::Mesh>("mesh", 1, true);
  reward_pub_ =
      nh_private_.advertise<tsdf_plusplus_msgs::Reward>("reward", 1, true);
  map_pub_ = nh_private_.advertise<tsdf_plusplus_msgs::SegmentedPointCloud>(
      "map", 1, true);
}

Controller::~Controller() { vizualizer_thread_.join(); }

void Controller::getConfigFromRosParam(const ros::NodeHandle &nh_private) {
  nh_private.param("world_frame", world_frame_, world_frame_);
  nh_private.param("sensor_frame", sensor_frame_, sensor_frame_);

  // Per-frame segmentation settings.
  nh_private.param("using_ground_truth_segmentation",
                   using_ground_truth_segmentation_,
                   using_ground_truth_segmentation_);

  // Object tracking settings.
  nh_private.param("object_tracking/enable", object_tracking_enabled_,
                   object_tracking_enabled_);
  nh_private.param("object_tracking/ground_truth_tracking",
                   ground_truth_tracking_, ground_truth_tracking_);

  // Human-readable semantic classes.
  nh_private.param<std::vector<std::string>>(
      "semantic_classes", semantic_classes_, semantic_classes_);

  // Mesh settings.
  nh_private.param("meshing/publish_mesh", publish_mesh_, publish_mesh_);
  nh_private.param("meshing/mesh_filename", mesh_filename_, mesh_filename_);

  std::vector<float> camera_intrinsics;
  nh_private.param<std::vector<float>>("camera_intrinsics", camera_intrinsics,
                                       camera_intrinsics);
  camera_intrinsics_ = Eigen::Map<Eigen::Matrix<float, 3, 3, Eigen::RowMajor>>(
      camera_intrinsics.data());
  nh_private.param<bool>("visualizer/write_frames_to_file",
                         write_frames_to_file_, write_frames_to_file_);

  nh_private.param("visualizer/export_path", export_path_, export_path_);

  bool verbose_log = false;
  nh_private.param<bool>("debug/verbose_log", verbose_log, verbose_log);

  if (verbose_log) {
    FLAGS_stderrthreshold = 0;
  }
}

void Controller::segmentPointcloudCallback(
    const tsdf_plusplus_msgs::SegmentedPointCloud::Ptr &segment_pcl_msg) {

  last_segment_msg_time_ = segment_pcl_msg->header.stamp;
  processSegmentPointcloud(segment_pcl_msg);

  if (current_frame_segments_.size() > 0u) {
    LOG(INFO) << "Integrating frame " << ++frame_number_ << " with timestamp "
              << std::fixed << last_segment_msg_time_.toSec();
    integrateFrame();

    if (write_frames_to_file_) {
      // Project the object map to 2D segmentation images.
      visualizer_->triggerScreenshot(frame_number_);
    }

    clearFrame();
  }

  publishReward();
  publishMap();
}

void Controller::resetCallback(const std_msgs::Bool::Ptr &reset_msg) {

  if (!reset_msg->data)
    return;

  // Reset Varibales to Reset Map State
  frame_number_ = 0u;
  *mesh_layer_updated_ = false; // ????
  *camera_extrinsics_ = Eigen::Matrix4f();

  last_segment_msg_time_ = ros::Time(0);

  map_->clear();
  mesh_layer_->clear();

  clearFrame();
}

void Controller::processSegmentPointcloud(
    const tsdf_plusplus_msgs::SegmentedPointCloud::Ptr &segment_pcl_msg) {
  // Look up transform from camera frame to world frame.
  if (lookupTransformTF(segment_pcl_msg->header.frame_id, world_frame_,
                        segment_pcl_msg->header.stamp, &T_G_C_)) {
    // Convert the PCL pointcloud into a Segment instance.
    voxblox::timing::Timer preprocess_timer("preprocess/segment");

    for (auto &segment_msg : segment_pcl_msg->segments) {
      // Horrible hack fix to fix color parsing colors in PCL.
      for (size_t d = 0u; d < segment_msg.pointcloud.fields.size(); ++d) {
        if (segment_msg.pointcloud.fields[d].name == std::string("rgb")) {
          segment_msg.pointcloud.fields[d].datatype =
              sensor_msgs::PointField::FLOAT32;
        }
      }

      Segment *segment;
      if (using_ground_truth_segmentation_) {
        pcl::PointCloud<GTInputPointType> pointcloud_pcl;
        pcl::moveFromROSMsg(segment_msg.pointcloud, pointcloud_pcl);

        segment = new Segment(pointcloud_pcl, T_G_C_, segment_msg.object_id);
      } else {
        pcl::PointCloud<InputPointType> pointcloud_pcl;
        pcl::moveFromROSMsg(segment_msg.pointcloud, pointcloud_pcl);

        segment = new Segment(pointcloud_pcl, T_G_C_);
      }

      // Add the segment to the collection of
      // segments observed in the current frame.
      current_frame_segments_.push_back(segment);

      if (ground_truth_tracking_) {
        // Convert Movement to Eigen Matrix
        Eigen::Matrix4f movement =
            Eigen::Map<Eigen::Matrix4f>(segment_msg.movement.data.data());
        current_frame_movements_.push_back({segment_msg.is_moved, movement});
      }

      if (!using_ground_truth_segmentation_) {
        integrator_->computeObjectOverlap(segment, &object_segment_overlap_);
      }
    }
    preprocess_timer.Stop();
  }
}

bool Controller::lookupTransformTF(const std::string &from_frame,
                                   const std::string &to_frame,
                                   const ros::Time &timestamp,
                                   Transformation *transform) {
  CHECK_NOTNULL(transform);

  tf::StampedTransform tf_transform;

  // Allow overwriting the TF frame for the sensor.
  std::string from_frame_modified = from_frame;
  if (!sensor_frame_.empty()) {
    from_frame_modified = sensor_frame_;
  }

  if (!tf_listener_.canTransform(to_frame, from_frame_modified, timestamp)) {
    ROS_ERROR_STREAM("Error getting TF transform from frame "
                     << from_frame_modified << " to frame " << to_frame << ".");
    return false;
  }

  try {
    tf_listener_.lookupTransform(to_frame, from_frame_modified, timestamp,
                                 tf_transform);
  } catch (tf::TransformException &ex) { // NOLINT
    ROS_ERROR_STREAM(
        "Error getting TF transform from sensor data: " << ex.what());
    return false;
  }

  tf::transformTFToKindr(tf_transform, transform);
  return true;
}

void Controller::integrateFrame() {
  pcl::console::TicToc tic_toc;

  if (!using_ground_truth_segmentation_) {
    voxblox::timing::Timer object_assignment_timer(
        "preprocess/assign_object_ids");

    tic_toc.tic();
    // All segments in the current frame have been processed and their
    // parwise overlap with objects in the map have been computed, now make an
    // ERRORrmed decision about which segment gets assigned which object_id.
    integrator_->assignObjectIds(&current_frame_segments_,
                                 &object_segment_overlap_,
                                 &object_merged_segments_);

    integrateSemanticClasses();

    object_assignment_timer.Stop();
  }

  {
    std::lock_guard<std::mutex> map_lock(map_mutex_);

    if (object_tracking_enabled_) {
      timing::Timer tracking_timer("all/track_and_update_poses");

      trackObjects();

      tracking_timer.Stop();
    }

    timing::Timer integrate_timer("all/integrate");
    tic_toc.tic();

    if (using_ground_truth_segmentation_) {
      for (Segment *segment : current_frame_segments_) {
        integrator_->integrateSegment(*segment);
      }

    } else {
      for (const auto &pair : object_merged_segments_) {
        Segment *segment = pair.second;
        segment->convertPointcloud();
        integrator_->integrateSegment(*segment);
      }
    }

    integrate_timer.Stop();

    if (using_ground_truth_segmentation_) {
      LOG(INFO) << "Integrated " << current_frame_segments_.size()
                << " segments in " << tic_toc.toc() << " ms. ";
    } else {
      LOG(INFO) << "Integrated " << object_merged_segments_.size()
                << " segments in " << tic_toc.toc() << " ms. ";
    }

    // Update the camera parameters of the visualizer to
    // fit its window to the current camera view.
    *camera_extrinsics_ = T_G_C_.getTransformationMatrix();
  }

  LOG(INFO) << "Timings: " << std::endl
            << voxblox::timing::Timing::Print() << std::endl;
}

void Controller::integrateSemanticClasses() {
  for (const auto &pair : object_merged_segments_) {
    Segment *segment = pair.second;
    if (segment->semantic_class_ == BackgroundClass) {
      continue;
    }
    ObjectVolume *object_volume =
        map_->getObjectVolumePtrById(segment->object_id_);
    if (object_volume) {
      object_volume->setSemanticClass(segment->semantic_class_);
    }
  }
}

void Controller::trackObjects() {
  // Track and update the pose of objects in the map.
  for (int i = 0; i < current_frame_segments_.size(); i++) {
    Segment *segment = current_frame_segments_[i];
    auto movement_info = current_frame_movements_[i];

    ObjectVolume *object_volume =
        map_->getObjectVolumePtrById(segment->object_id_);

    if (object_volume) {
      if (using_ground_truth_segmentation_) {
        // TODO(margaritaG): parametrize this nicely.
        // Because ground truth segmentation only provide object instance IDs
        // and no semantics, we use thresholds on the object segment size
        // to differentiate between small moving foreground objects and
        // large static background structures.
        if (ground_truth_tracking_) {
          if (!movement_info.first) {
            LOG(INFO) << "Skipping pose tracking because object is static. ID: "
                      << segment->object_id_;
            continue;
          }
        } else {
          if (segment->object_id_ % 2 == 0 ||
              segment->points_C_.size() > 100000) {
            LOG(INFO) << "Skipping pose tracking of object segment as its "
                         "size is too large or too low. (number of points: "
                      << segment->points_C_.size() << ").";
            continue;
          }
        }
      } else {
        // Only track objects that have been at least
        // once semantically annotated.
        if (segment->semantic_class_ == BackgroundClass &&
            object_volume->getSemanticClass() == BackgroundClass) {
          continue;
        }
        // TODO(margaritaG): parametrize this nicely.
        if (segment->points_C_.size() > 100000) {
          LOG(INFO) << "Skipping pose tracking of object segment as its "
                       "size is too large. (number of points: "
                    << segment->points_C_.size() << ").";
          continue;
        }
      }

      timing::Timer icp_timer("icp/align");

      Eigen::Matrix4f G_T_O_S = Eigen::Matrix4f::Identity();
      Transformation T_O_S;

      if (ground_truth_tracking_) {
        G_T_O_S = movement_info.second;
      } else {
        timing::Timer icp_preprocess_timer("icp/preprocess");

        Transformation T_G_O = object_volume->getPose();

        // Segment extracted from the current frame.
        pcl::PointCloud<PointTypeNormal>::Ptr C_segment_pcl_cloud(
            new pcl::PointCloud<PointTypeNormal>);
        pcl::copyPointCloud(segment->pointcloud_, *C_segment_pcl_cloud);

        // Object model stored in the map.
        pcl::PointCloud<PointTypeNormal>::Ptr G_model_pcl_cloud(
            new pcl::PointCloud<PointTypeNormal>);

        // Mesh the object model and extract a point cloud as the mesh vertices.
        voxblox::MeshIntegratorConfig config;
        static constexpr bool kConnectedMesh = true;
        // TODO(margaritaG): optimize this conversion.
        convertVoxelGridToPointCloud(*object_volume->getTsdfLayerPtr(), config,
                                     G_model_pcl_cloud.get(), kConnectedMesh);

        // If the resulting point cloud is empty, skip pose tracking.
        if (G_model_pcl_cloud->points.size() == 0) {
          continue;
        }

        icp_preprocess_timer.Stop();

        pcl::PointCloud<PointTypeNormal>::Ptr G_segment_pcl_cloud(
            new pcl::PointCloud<PointTypeNormal>);
        // Transform segment cloud from camera frame to global frame.
        pcl::transformPointCloud(*C_segment_pcl_cloud, *G_segment_pcl_cloud,
                                 segment->T_G_C_.getTransformationMatrix());

        Eigen::Matrix4f G_T_S_O = Eigen::Matrix4f::Identity();

        // Align the source: segment point cloud to the target: object model.
        bool success = icp_->align(G_segment_pcl_cloud, G_model_pcl_cloud,
                                   Eigen::Matrix4f::Identity(), &G_T_S_O);

        if (!success) {
          LOG(INFO) << "ICP has not converged, assuming object did not "
                       "move.";
          G_T_S_O = Eigen::Matrix4f::Identity();
        }

        G_T_O_S = G_T_S_O.inverse();
      }

      T_O_S = Transformation().constructAndRenormalizeRotation(G_T_O_S);

      icp_timer.Stop();

      timing::Timer move_timer("icp/move");

      map_->transformLayer(segment->object_id_, T_O_S);
      object_volume->accumulateTransform(T_O_S);

      move_timer.Stop();
    }
  }
}

void Controller::clearFrame() {
  for (Segment *segment : current_frame_segments_) {
    delete segment;
  }

  current_frame_segments_.clear();
  current_frame_movements_.clear();
  object_segment_overlap_.clear();
  object_merged_segments_.clear();
}

void Controller::updateMeshEvent(const ros::TimerEvent &event) {
  std::lock_guard<std::mutex> mesh_layer_lock(*mesh_layer_mutex_);
  std::lock_guard<std::mutex> map_lock(map_mutex_);

  timing::Timer update_mesh_timer("mesh/update");

  constexpr bool only_mesh_updated_blocks = true;
  constexpr bool clear_updated_flag = true;

  pcl::console::TicToc tic_toc;

  tic_toc.tic();

  *mesh_layer_updated_ = mesh_integrator_->generateMesh(
                             only_mesh_updated_blocks, clear_updated_flag) ||
                         *mesh_layer_updated_;

  update_mesh_timer.Stop();

  if (publish_mesh_) {
    timing::Timer mesh_msg_timer("mesh/publish_msg");

    voxblox_msgs::Mesh mesh_msg;
    generateVoxbloxMeshMsg(mesh_layer_.get(), voxblox::ColorMode::kColor,
                           &mesh_msg);
    mesh_msg.header.frame_id = world_frame_;
    mesh_pub_.publish(mesh_msg);

    mesh_msg_timer.Stop();
  }
}

bool Controller::generateMeshCallback(std_srvs::Empty::Request & /*request*/,
                                      std_srvs::Empty::Response &
                                      /*response*/) {
  {
    std::lock_guard<std::mutex> mesh_layer_lock(*mesh_layer_mutex_);
    {
      std::lock_guard<std::mutex> map_lock(map_mutex_);

      timing::Timer generate_mesh_timer("mesh/generate");

      constexpr bool only_mesh_updated_blocks = false;
      constexpr bool clear_updated_flag = true;
      mesh_integrator_->generateMesh(only_mesh_updated_blocks,
                                     clear_updated_flag);

      *mesh_layer_updated_ = true;

      generate_mesh_timer.Stop();
    }

    if (publish_mesh_) {
      timing::Timer mesh_msg_timer("mesh/publish_msg");

      voxblox_msgs::Mesh mesh_msg;
      generateVoxbloxMeshMsg(mesh_layer_.get(), voxblox::ColorMode::kColor,
                             &mesh_msg);
      mesh_msg.header.frame_id = world_frame_;
      mesh_pub_.publish(mesh_msg);

      mesh_msg_timer.Stop();
    }

    if (!mesh_filename_.empty()) {
      const bool success = outputMeshLayerAsPly(mesh_filename_, *mesh_layer_);
      if (success) {
        LOG(INFO) << "Output file as PLY: " << mesh_filename_.c_str();
      } else {
        LOG(INFO) << "Failed to output mesh as PLY: " << mesh_filename_.c_str();
      }
    }
  }

  return true;
}

bool Controller::saveObjectsCallback(std_srvs::Empty::Request & /*request*/,
                                     std_srvs::Empty::Response &
                                     /*response*/) {
  {
    std::lock_guard<std::mutex> map_lock(map_mutex_);

    std::map<ObjectID, ObjectVolume *> *object_volumes =
        map_->getObjectVolumesPtr();

    for (const auto &pair : *object_volumes) {
      if (!using_ground_truth_segmentation_ &&
          pair.second->getSemanticClass() == BackgroundClass &&
          pair.first != 2u) {
        continue;
      }
      CHECK_EQ(makePath("tpp_objects", 0777), 0);

      std::string mesh_filename =
          "tpp_objects/tpp_object_" + std::to_string(pair.first) + ".ply";

      bool success = voxblox::io::outputLayerAsPly(
          *pair.second->getTsdfLayerPtr(), mesh_filename,
          voxblox::io::PlyOutputTypes::kSdfIsosurface);

      if (success) {
        LOG(INFO) << "Output object file as PLY: " << mesh_filename.c_str();
      } else {
        LOG(INFO) << "Failed to output mesh as PLY:" << mesh_filename.c_str();
      }
    }
  }

  return true;
}

bool Controller::publishReward() {

  {
    std::lock_guard<std::mutex> map_lock(map_mutex_);
    Layer<MOVoxel> *global_map = map_->getMapLayerPtr();
    // Get the indices of all allocated voxels
    BlockIndexList global_map_blocks;
    global_map->getAllAllocatedBlocks(&global_map_blocks);
    std::map<ObjectID, ObjectVolume *> *object_volumes = map_->getObjectVolumesPtr();

    // Initialize Reward Message
    tsdf_plusplus_msgs::Reward msg;
    msg.number_of_objects = 0;
    msg.number_of_voxels = 0;
    msg.number_of_free_voxels = 0;
    msg.number_of_occupied_voxels = 0;
    msg.number_of_unknown_voxels = 0;

    // Calculate Number of Objects
    for (const auto &pair : *object_volumes) {
      msg.number_of_objects++;      
    }

    for (const BlockIndex &block_index : global_map_blocks){
      auto global_map_block =  global_map->getBlockPtrByIndex(block_index);

      // Iterate over all voxels inside the block
      for(int i=0; i<global_map_block->num_voxels(); i++){
        // Increase Number of Voxels
        msg.number_of_voxels++;

        // Get Global Map Voxel
        auto voxel = global_map_block->getVoxelByLinearIndex(i);
        auto voxel_center = global_map_block->computeCoordinatesFromLinearIndex(i);

        if(voxel.active_object.object_id == 0u){
            msg.number_of_unknown_voxels++; // Check these voxels
            continue;
        }

        // Get Object Volume Voxel
        auto object_volume = (*object_volumes)[voxel.active_object.object_id];
        auto object_volume_block = object_volume->getTsdfLayerPtr();
        auto tsdf_voxel = object_volume_block->getVoxelPtrByCoordinates(voxel_center);

        if(tsdf_voxel->weight < 1e-6){
          msg.number_of_unknown_voxels++;
        }else{
          if(abs(tsdf_voxel->distance) < (object_volume_block->voxel_size()/2)){
            msg.number_of_occupied_voxels++;
          }else{
            msg.number_of_free_voxels++;
          }
        }

      }
    }

    reward_pub_.publish(msg);
  }

  return true;
}

bool Controller::publishMap() {

  {
    std::lock_guard<std::mutex> map_lock(map_mutex_);

    tsdf_plusplus_msgs::SegmentedPointCloud msg;
    msg.header.frame_id = world_frame_;
    msg.header.stamp = last_segment_msg_time_;

    std::map<ObjectID, ObjectVolume *> *object_volumes =
        map_->getObjectVolumesPtr();

    for (const auto &pair : *object_volumes) {

      tsdf_plusplus_msgs::MovementPointCloud pc_msg;
      pc_msg.object_id = pair.first;

      pcl::PointCloud<pcl::PointXYZ> pcl_pointcloud;

      // Extract TSDF Layer of the Object
      Layer<TsdfVoxel> *object_layer = pair.second->getTsdfLayerPtr();
      // Get the indices of all allocated voxels
      BlockIndexList all_object_blocks;
      object_layer->getAllAllocatedBlocks(&all_object_blocks);
      // Extract Positions of Each Voxel
      for (const BlockIndex &block_index : all_object_blocks) {
        const voxblox::Point c = getCenterPointFromGridIndex(
            block_index, object_layer->block_size());
        pcl_pointcloud.push_back(pcl::PointXYZ(c(0, 0), c(1, 0), c(2, 0)));

        // Get Voxels of Each Object
        // auto block = object_layer->getBlockPtrByCoordinates(c);
        // for(int i=0; i<block->num_voxels(); i++){
        //   auto voxel = block->getVoxelByLinearIndex(i);
        // }
      }

      sensor_msgs::PointCloud2 object_msg;
      pcl::toROSMsg(pcl_pointcloud, object_msg);
      pc_msg.pointcloud = object_msg;

      msg.segments.push_back(pc_msg);
    }

    map_pub_.publish(msg);
  }

  return true;
}

bool Controller::removeObjectsCallback(std_srvs::Empty::Request & /*request*/,
                                       std_srvs::Empty::Response &
                                       /*response*/) {
  {
    std::lock_guard<std::mutex> map_lock(map_mutex_);

    std::map<ObjectID, ObjectVolume *> *object_volumes =
        map_->getObjectVolumesPtr();

    // Remove all Objects
    for (const auto &pair : *object_volumes) {
      map_->removeObject(pair.first);
    }
    *mesh_layer_updated_ = true;

    if (write_frames_to_file_) {
      // Project the object map to 2D segmentation images.
      visualizer_->triggerScreenshot(frame_number_);
    }
  }
  return true;
}
