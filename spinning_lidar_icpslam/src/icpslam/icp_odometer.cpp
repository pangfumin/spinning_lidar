
#include "utils/geometric_utils.h"
#include "utils/messaging_utils.h"
#include "icpslam/icp_odometer.h"

#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <geometry_msgs/Point.h>
#include <geometry_msgs/Quaternion.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/Path.h>
#include <nav_msgs/Odometry.h>
#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>
#include <tf/transform_datatypes.h>

#include <pcl_ros/impl/transforms.hpp>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/registration/icp.h>
#include <pcl/registration/gicp.h>
#include <pcl/filters/voxel_grid.h>
#include <Eigen/Dense>
#include <Eigen/Geometry>



ICPOdometer::ICPOdometer(ros::NodeHandle nh) :
	nh_(nh),
	prev_cloud_(new pcl::PointCloud<pcl::PointXYZ>()), curr_cloud_(new pcl::PointCloud<pcl::PointXYZ>())
{
	robot_odom_inited_ = false;
	init();	
}

void ICPOdometer::init()
{
	loadParameters();
	advertisePublishers();
	registerSubscribers();

	ROS_INFO("ICP odometer initialized");
}

void ICPOdometer::loadParameters()
{
	nh_.param("verbosity_level_", verbosity_level_, 1);

	// TF frames
	nh_.param("map_frame", map_frame_, std::string("map"));
	nh_.param("odom_frame", odom_frame_, std::string("odom"));
	nh_.param("robot_frame", robot_frame_, std::string("base_link"));
	nh_.param("laser_frame", laser_frame_, std::string("laser"));

	// Input robot odometry and point cloud topics
	nh_.param("assembled_cloud_topic", assembled_cloud_topic_, std::string("spinning_lidar/assembled_cloud"));
	nh_.param("robot_odom_topic", robot_odom_topic_, std::string("/odometry/filtered"));
	nh_.param("robot_odom_path_topic", robot_odom_path_topic_, std::string("icpslam/robot_odom_path"));

	// ICP odometry debug topics
	if(verbosity_level_ >=1)
	{
		nh_.param("prev_cloud_topic", prev_cloud_topic_, std::string("icpslam/prev_cloud"));
		nh_.param("aligned_cloud_topic", aligned_cloud_topic_, std::string("icpslam/aligned_cloud"));

		nh_.param("icp_odom_topic", icp_odom_topic_, std::string("icpslam/odom"));
		nh_.param("icp_odom_path_topic", icp_odom_path_topic_, std::string("icpslam/icp_odom_path"));
	}
}

void ICPOdometer::advertisePublishers()
{
	robot_odom_path_pub_ = nh_.advertise<nav_msgs::Path>(robot_odom_path_topic_, 1);
	
	// ICP odometry debug topics
	if(verbosity_level_ >=1)
	{
		prev_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2>(prev_cloud_topic_, 1);
		aligned_cloud_pub_ = nh_.advertise<sensor_msgs::PointCloud2>(aligned_cloud_topic_, 1);

		icp_odom_pub_ = nh_.advertise<nav_msgs::Odometry>(icp_odom_topic_, 1);
		icp_odom_path_pub_ = nh_.advertise<nav_msgs::Path>(icp_odom_path_topic_, 1);
	}
}

void ICPOdometer::registerSubscribers()
{
	tf::TransformListener tf_listener;
	tf_listener_ptr_ = &tf_listener;
	tf::TransformBroadcaster tf_broadcaster;
	tf_broadcaster_ptr_ = &tf_broadcaster;

	robot_odometry_sub_ = nh_.subscribe(robot_odom_topic_, 1, &ICPOdometer::robotOdometryCallback, this);
	assembled_cloud_sub_ = nh_.subscribe(assembled_cloud_topic_, 1, &ICPOdometer::assembledCloudCallback, this);
}

bool ICPOdometer::isOdomReady()
{
	return robot_odom_inited_;
}

Pose6DOF ICPOdometer::getLatestPoseRobotOdometry() 
{ 
  return robot_odom_poses_.back(); 
} 

Pose6DOF ICPOdometer::getLatestPoseICPOdometry() 
{ 
  return icp_odom_poses_.back(); 
} 

void ICPOdometer::getLatestPoseRobotOdometry(Pose6DOF *pose)
{
	*pose = robot_odom_poses_.back();
}

void ICPOdometer::getLatestPoseICPOdometry(Pose6DOF *pose)
{
	*pose = icp_odom_poses_.back();
}

void ICPOdometer::getLatestCloud(pcl::PointCloud<pcl::PointXYZ>::Ptr cloud, Pose6DOF *pose)
{
	*cloud = *prev_cloud_;
	*pose = icp_odom_poses_.back();
}

void ICPOdometer::robotOdometryCallback(const nav_msgs::Odometry::ConstPtr& robot_odom_msg)
{
	// ROS_INFO("Robot odometry callback!");
	Pose6DOF pose;
	pose.time_stamp = robot_odom_msg->header.stamp;
	pose.pos = getTranslationFromROSPose(robot_odom_msg->pose.pose);
	pose.rot = getQuaternionFromROSPose(robot_odom_msg->pose.pose);
	pose.cov = getCovarianceFromROSPoseWithCovariance(robot_odom_msg->pose);
	robot_odom_poses_.push_back(pose);

	int num_poses = robot_odom_path_.poses.size();
	if(num_poses > 0)
	{
		geometry_msgs::Pose prev_pose = robot_odom_path_.poses[num_poses-1].pose;
		double dist = lengthOfVector(differenceBetweenPoses(prev_pose, robot_odom_msg->pose.pose));
		if(dist < POSE_DIST_THRESH)
		{
			return;
		}
	}
	else
	{
		icp_odom_poses_.push_back(pose);
		robot_odom_inited_ = true;
		insertPoseInPath(robot_odom_msg->pose.pose, odom_frame_, robot_odom_msg->header.stamp, icp_odom_path_);
	}

	if(verbosity_level_ >= 2)
	{
		std::cout << "Robot odometry position = " << getStringFromVector3d(pose.pos) << std::endl;
		std::cout << "Robot odometry rotation = " << getStringFromQuaternion(pose.rot) << std::endl;
	}

	insertPoseInPath(robot_odom_msg->pose.pose, odom_frame_, robot_odom_msg->header.stamp, robot_odom_path_);

	robot_odom_path_.header.stamp = ros::Time().now();
	robot_odom_path_.header.frame_id = odom_frame_;
	robot_odom_path_pub_.publish(robot_odom_path_);
}

void ICPOdometer::mapTransformCallback(const ros::TimerEvent&)
{
	if(robot_odom_inited_)
	{
		Pose6DOF pose = getLatestPoseICPOdometry();

		// Publish transform between odom and map
		tf::Transform transform;
		transform.setOrigin( tf::Vector3(pose.pos(0), pose.pos(1), pose.pos(2)) );
		transform.setRotation( tf::Quaternion(pose.rot.x(), pose.rot.y(), pose.rot.z(), pose.rot.w()) );				
		tf_broadcaster_ptr_->sendTransform(tf::StampedTransform(transform, ros::Time::now(), map_frame_, odom_frame_));
	}
}

void ICPOdometer::updateICPOdometry(Eigen::Matrix4f T)
{
	// ROS_INFO("ICP odometry update!");
	Eigen::Vector3d t = getTranslationFromTMatrix(T);
	Eigen::Quaterniond q = getQuaternionFromTMatrix(T);

	Pose6DOF prev_pose = getLatestPoseRobotOdometry();
	Pose6DOF new_pose;
	new_pose.time_stamp = ros::Time().now();
	new_pose.pos = prev_pose.pos + prev_pose.rot.toRotationMatrix() * t;
	new_pose.rot = prev_pose.rot * q;
	new_pose.rot.normalize();

	// double dist = (prev_pos - curr_pos).norm();
	// if(dist < POSE_DIST_THRESH)
	{	
		icp_odom_poses_.push_back(new_pose);
		insertPoseInPath(new_pose.pos, new_pose.rot, odom_frame_, ros::Time().now(), icp_odom_path_);
		publishOdometry(new_pose.pos, new_pose.rot, odom_frame_, robot_frame_, ros::Time().now(), &icp_odom_pub_);
		icp_odom_path_.header.stamp = ros::Time().now();
		icp_odom_path_.header.frame_id = odom_frame_;
		icp_odom_path_pub_.publish(icp_odom_path_);
	}

	if(verbosity_level_ >= 2)
	{
		std::cout << "Initial position = " << getStringFromVector3d(icp_odom_poses_[0].pos) << std::endl;
		std::cout << "Initial rotation = " << getStringFromQuaternion(icp_odom_poses_[0].rot) << std::endl;
		std::cout << "Cloud translation = " << getStringFromVector3d(t) << std::endl;
		std::cout << "Cloud rotation = " << getStringFromQuaternion(q) << std::endl;
		std::cout << "ICP odometry position = " << getStringFromVector3d(new_pose.pos) << std::endl;
		std::cout << "ICP odometry rotation = " << getStringFromQuaternion(new_pose.rot) << std::endl;
		std::cout << std::endl;
	}
}

void ICPOdometer::assembledCloudCallback(const sensor_msgs::PointCloud2::ConstPtr& cloud_msg)
{
	// ROS_INFO("Cloud callback!");
	pcl::PointCloud<pcl::PointXYZ>::Ptr input_cloud(new pcl::PointCloud<pcl::PointXYZ>());
	pcl::fromROSMsg(*cloud_msg, *input_cloud);
	pcl::VoxelGrid<pcl::PointXYZ> voxel_filter;
	voxel_filter.setInputCloud(input_cloud);
	voxel_filter.setLeafSize(0.05, 0.05, 0.05);
	voxel_filter.filter(*curr_cloud_);

	if(prev_cloud_->points.size() > 0 && robot_odom_inited_)
	{
		// Registration
		// GICP is said to be better, but what about NICP from Serafin?
		pcl::GeneralizedIterativeClosestPoint<pcl::PointXYZ, pcl::PointXYZ> icp;
		icp.setInputSource(prev_cloud_);
		icp.setInputTarget(curr_cloud_);
		icp.setMaximumIterations(ICP_MAX_ITERS);
		icp.setTransformationEpsilon(ICP_EPSILON);
		icp.setMaxCorrespondenceDistance(ICP_MAX_CORR_DIST);
		icp.setRANSACIterations(0);
		// icp.setMaximumOptimizerIterations(50);

		pcl::PointCloud<pcl::PointXYZ>::Ptr aligned_cloud(new pcl::PointCloud<pcl::PointXYZ>());
		icp.align(*aligned_cloud);
		Eigen::Matrix4f T = icp.getFinalTransformation();
		
		if(icp.hasConverged())
		{
			updateICPOdometry(T);

			// Publishing for debug
			if(verbosity_level_ >= 1)
			{
				// ROS_INFO("# Cloud frame: %s", cloud_msg->header.frame_id.c_str());
				// ROS_INFO("##   Registration");
				// std::cout << "### ICP finished! \nConverged: " << icp.hasConverged() << " \nScore: " << icp.getFitnessScore() << std::endl;
				// std::cout << T << std::endl;
				
				publishPointCloud(prev_cloud_, cloud_msg->header.frame_id, ros::Time().now(), &prev_cloud_pub_);
				publishPointCloud(aligned_cloud, cloud_msg->header.frame_id, ros::Time().now(), &aligned_cloud_pub_);
			}
		}

	}

	*prev_cloud_ = *curr_cloud_;
}