#include "jsk_pcl_ros/depth_image_creator.h"

void jsk_pcl_ros::DepthImageCreator::onInit () {
  NODELET_INFO("[%s::onInit]", getName().c_str());
  PCLNodelet::onInit();

  // scale_depth
  pnh_->param("scale_depth", scale_depth, 1.0);
  ROS_INFO("scale_depth : %f", scale_depth);

  // use fixed transform
  pnh_->param("use_fixed_transform", use_fixed_transform, false);
  ROS_INFO("use_fixed_transform : %d", use_fixed_transform);

  pnh_->param("use_service", use_service, false);
  ROS_INFO("use_service : %d", use_service);

  pnh_->param("use_asynchronous", use_asynchronous, false);
  ROS_INFO("use_asynchronous : %d", use_asynchronous);

  pnh_->param("use_approximate", use_approximate, false);
  ROS_INFO("use_approximate : %d", use_approximate);

  pnh_->param("info_throttle", info_throttle_, 0);
  info_counter_ = 0;
 
  // set transformation
  double trans_pos[3];
  double trans_quat[4];
  trans_pos[0] = trans_pos[1] = trans_pos[2] = 0;
  if (pnh_->hasParam("translation")) {
    XmlRpc::XmlRpcValue param_val;
    pnh_->getParam("translation", param_val);
    if (param_val.getType() == XmlRpc::XmlRpcValue::TypeArray && param_val.size() == 3) {
      trans_pos[0] = param_val[0];
      trans_pos[1] = param_val[1];
      trans_pos[2] = param_val[2];
    }
    ROS_INFO("translation : [%f, %f, %f]", trans_pos[0], trans_pos[1], trans_pos[2]);
  }
  trans_quat[0] = trans_quat[1] = trans_quat[2] = 0; trans_quat[3] = 1;
  if (pnh_->hasParam("rotation")) {
    XmlRpc::XmlRpcValue param_val;
    pnh_->getParam("rotation", param_val);
    if (param_val.getType() == XmlRpc::XmlRpcValue::TypeArray && param_val.size() == 4) {
      trans_quat[0] = param_val[0];
      trans_quat[1] = param_val[1];
      trans_quat[2] = param_val[2];
      trans_quat[3] = param_val[3];
    }
    ROS_INFO("rotation : [%f, %f, %f, %f]",
             trans_quat[0], trans_quat[1],
             trans_quat[2], trans_quat[3]);
  }
  tf::Quaternion btq(trans_quat[0], trans_quat[1], trans_quat[2], trans_quat[3]);
  tf::Vector3 btp(trans_pos[0], trans_pos[1], trans_pos[2]);
  fixed_transform.setOrigin(btp);
  fixed_transform.setRotation(btq);

  pub_image_ = pnh_->advertise<sensor_msgs::Image> ("output", max_queue_size_);
  pub_cloud_ = pnh_->advertise<PointCloud> ("output_cloud", max_queue_size_);
  pub_disp_image_ = pnh_->advertise<stereo_msgs::DisparityImage> ("output_disp", max_queue_size_);

  if (!use_service) {
    if (use_asynchronous) {
      sub_as_info_ = pnh_->subscribe<sensor_msgs::CameraInfo> ("info", max_queue_size_,
                                                               &DepthImageCreator::callback_info, this);
      sub_as_cloud_ = pnh_->subscribe<sensor_msgs::PointCloud2> ("input", max_queue_size_,
                                                                 &DepthImageCreator::callback_cloud, this);
    } else {
      sub_info_.subscribe(*pnh_, "info", max_queue_size_);
      sub_cloud_.subscribe(*pnh_, "input", max_queue_size_);

      if (use_approximate) {
        sync_inputs_a_ = boost::make_shared <message_filters::Synchronizer<message_filters::sync_policies::ApproximateTime<sensor_msgs::CameraInfo, sensor_msgs::PointCloud2> > > (max_queue_size_);
        sync_inputs_a_->connectInput (sub_info_, sub_cloud_);
        sync_inputs_a_->registerCallback (bind (&DepthImageCreator::callback_sync, this, _1, _2));
      } else {
        sync_inputs_e_ = boost::make_shared <message_filters::Synchronizer<message_filters::sync_policies::ExactTime<sensor_msgs::CameraInfo, sensor_msgs::PointCloud2> > > (max_queue_size_);
        sync_inputs_e_->connectInput (sub_info_, sub_cloud_);
        sync_inputs_e_->registerCallback (bind (&DepthImageCreator::callback_sync, this, _1, _2));
      }
    }
  } else {
    // not continuous
    sub_as_info_ = pnh_->subscribe<sensor_msgs::CameraInfo> ("info", max_queue_size_,
                                                             &DepthImageCreator::callback_info, this);
    service_ = pnh_->advertiseService("set_point_cloud",
                                      &DepthImageCreator::service_cb, this);
  }
}

jsk_pcl_ros::DepthImageCreator::~DepthImageCreator() { }

bool jsk_pcl_ros::DepthImageCreator::service_cb (std_srvs::Empty::Request &req,
                                                 std_srvs::Empty::Response &res) {
  return true;
}

void jsk_pcl_ros::DepthImageCreator::callback_sync(const sensor_msgs::CameraInfoConstPtr& info,
                                                   const sensor_msgs::PointCloud2ConstPtr& pcloud2) {
  ROS_DEBUG("DepthImageCreator::callback_sync");
  publish_points(info, pcloud2);
}

void jsk_pcl_ros::DepthImageCreator::callback_cloud(const sensor_msgs::PointCloud2ConstPtr& pcloud2) {
  ROS_DEBUG("DepthImageCreator::callback_cloud");
  boost::mutex::scoped_lock lock(this->mutex_points);
  points_ptr_ = pcloud2;
}

void jsk_pcl_ros::DepthImageCreator::callback_info(const sensor_msgs::CameraInfoConstPtr& info) {
  ROS_DEBUG("DepthImageCreator::callback_info");
  boost::mutex::scoped_lock lock(this->mutex_points);
  if( info_counter_++ >= info_throttle_ ) {
    info_counter_ = 0;
  } else {
    return;
  }
  publish_points(info, points_ptr_);
}

void jsk_pcl_ros::DepthImageCreator::publish_points(const sensor_msgs::CameraInfoConstPtr& info,
                                                    const sensor_msgs::PointCloud2ConstPtr& pcloud2) {
  ROS_DEBUG("DepthImageCreator::publish_points");
  if (!pcloud2)  return;
  bool proc_cloud = true, proc_image = true, proc_disp = true;
  if ( pub_cloud_.getNumSubscribers()==0 ) {
    proc_cloud = false;
  }
  if ( pub_image_.getNumSubscribers()==0 ) {
    proc_image = false;
  }
  if ( pub_disp_image_.getNumSubscribers()==0 ) {
    proc_disp = false;
  }
  if( !proc_cloud && !proc_image && !proc_disp) return;

  int width = info->width;
  int height = info->height;
  float fx = info->P[0];
  float cx = info->P[2];
  float tx = info->P[3];
  float fy = info->P[5];
  float cy = info->P[6];

  Eigen::Affine3f sensorPose;
  {
    tf::StampedTransform transform;
    if(use_fixed_transform) {
      transform = fixed_transform;
    } else {
      try {
	tf_listener_.waitForTransform(pcloud2->header.frame_id,
				      info->header.frame_id,
				      info->header.stamp,
				      ros::Duration(0.001));
        tf_listener_.lookupTransform(pcloud2->header.frame_id,
                                     info->header.frame_id,
                                     info->header.stamp, transform);
      }
      catch ( std::runtime_error e ) {
        ROS_ERROR("%s",e.what());
        return;
      }
    }
    tf::Vector3 p = transform.getOrigin();
    tf::Quaternion q = transform.getRotation();
    sensorPose = (Eigen::Affine3f)Eigen::Translation3f(p.getX(), p.getY(), p.getZ());
    Eigen::Quaternion<float> rot(q.getW(), q.getX(), q.getY(), q.getZ());
    sensorPose = sensorPose * rot;

    if (tx != 0.0) {
      Eigen::Affine3f trans = (Eigen::Affine3f)Eigen::Translation3f(-tx/fx , 0, 0);
      sensorPose = sensorPose * trans;
    }
#if 0 // debug print
    ROS_INFO("%f %f %f %f %f %f %f %f %f, %f %f %f",
             sensorPose(0,0), sensorPose(0,1), sensorPose(0,2),
             sensorPose(1,0), sensorPose(1,1), sensorPose(1,2),
             sensorPose(2,0), sensorPose(2,1), sensorPose(2,2),
             sensorPose(0,3), sensorPose(1,3), sensorPose(2,3));
#endif
  }

  PointCloud pointCloud;
  pcl::RangeImagePlanar rangeImageP;
  {
    // code here is dirty, some bag is in RangeImagePlanar
    PointCloud tpc;
    pcl::fromROSMsg(*pcloud2, tpc);

    Eigen::Affine3f inv;
#if ( PCL_MAJOR_VERSION >= 1 && PCL_MINOR_VERSION >= 5 )
    inv = sensorPose.inverse();
    pcl::transformPointCloud< Point > (tpc, pointCloud, inv);
#else
    pcl::getInverse(sensorPose, inv);
    pcl::getTransformedPointCloud<PointCloud> (tpc, inv, pointCloud);
#endif

    Eigen::Affine3f dummytrans;
    dummytrans.setIdentity();
    rangeImageP.createFromPointCloudWithFixedSize (pointCloud,
                                                   width/scale_depth, height/scale_depth,
                                                   cx/scale_depth, cy/scale_depth,
                                                   fx/scale_depth, fy/scale_depth,
                                                   dummytrans); //sensorPose);
  }

  cv::Mat mat(rangeImageP.height, rangeImageP.width, CV_32FC1);
  float *tmpf = (float *)mat.ptr();
  for(unsigned int i = 0; i < rangeImageP.height*rangeImageP.width; i++) {
    tmpf[i] = rangeImageP.points[i].z;
  }

  if(scale_depth != 1.0) {
    cv::Mat tmpmat(info->height, info->width, CV_32FC1);
    cv::resize(mat, tmpmat, cv::Size(info->width, info->height)); // LINEAR
    //cv::resize(mat, tmpmat, cv::Size(info->width, info->height), 0.0, 0.0, cv::INTER_NEAREST);
    mat = tmpmat;
  }

  if (proc_image) {
    sensor_msgs::Image pubimg;
    pubimg.header = info->header;
    pubimg.width = info->width;
    pubimg.height = info->height;
    pubimg.encoding = "32FC1";
    pubimg.step = sizeof(float)*info->width;
    pubimg.data.resize(sizeof(float)*info->width*info->height);

    // publish image
    memcpy(&(pubimg.data[0]), mat.ptr(), sizeof(float)*info->height*info->width);
    pub_image_.publish(boost::make_shared<sensor_msgs::Image>(pubimg));
  }

  if(proc_cloud || proc_disp) {
    // publish point cloud
    pcl::RangeImagePlanar rangeImagePP;
    rangeImagePP.setDepthImage ((float *)mat.ptr(),
                                width, height,
                                cx, cy, fx, fy);
#if PCL_MAJOR_VERSION == 1 && PCL_MINOR_VERSION >= 7
    rangeImagePP.header = pcl_conversions::toPCL(info->header);
#else
    rangeImagePP.header = info->header;
#endif
    if(proc_cloud) {
      pub_cloud_.publish(boost::make_shared<pcl::PointCloud<pcl::PointWithRange > >
                         ( (pcl::PointCloud<pcl::PointWithRange>)rangeImagePP) );
    }

    if(proc_disp) {
      stereo_msgs::DisparityImage disp;
#if PCL_MAJOR_VERSION == 1 && PCL_MINOR_VERSION >= 7
      disp.header = pcl_conversions::fromPCL(rangeImagePP.header);
#else
      disp.header = rangeImagePP.header;
#endif
      disp.image.encoding  = sensor_msgs::image_encodings::TYPE_32FC1;
      disp.image.height    = rangeImagePP.height;
      disp.image.width     = rangeImagePP.width;
      disp.image.step      = disp.image.width * sizeof(float);
      disp.f = fx; disp.T = 0.075;
      disp.min_disparity = 0;
      disp.max_disparity = disp.T * disp.f / 0.3;
      disp.delta_d = 0.125;

      disp.image.data.resize (disp.image.height * disp.image.step);
      float *data = reinterpret_cast<float*> (&disp.image.data[0]);

      float normalization_factor = disp.f * disp.T;
      for (int y = 0; y < (int)rangeImagePP.height; y++ ) {
        for (int x = 0; x < (int)rangeImagePP.width; x++ ) {
          pcl::PointWithRange p = rangeImagePP.getPoint(x,y);
          data[y*disp.image.width+x] = normalization_factor / p.z;
        }
      }
      pub_disp_image_.publish(boost::make_shared<stereo_msgs::DisparityImage> (disp));
    }
  }
}

typedef jsk_pcl_ros::DepthImageCreator DepthImageCreator;
PLUGINLIB_DECLARE_CLASS (jsk_pcl, DepthImageCreator, DepthImageCreator, nodelet::Nodelet);
