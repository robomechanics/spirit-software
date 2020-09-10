#include "ekf_estimator/ekf_estimator.h"

EKFEstimator::EKFEstimator(ros::NodeHandle nh) {
	nh_ = nh;

	// Load topic names from rosparam server
	std::string joint_encoder_topic, imu_topic, state_estimate_topic;
	nh.param<std::string>("topics/joint_encoder", "/joint_encoder");
	nh.param<std::string>("topics/imu", imu_topic, "/imu");
	nh.param<std::string>("topics/state_estimate", state_estimate_topic, "/state_estimate");

	// Setup pubs and subs
	joint_encoder_sub_ = nh_.subscribe(joint_encoder_topic,1,&EKFEstimator::jointEncoderCallback, this);
	imu_sub_ = nh_.subscribe(imu_topic,1,&EKFEstimator::imuCallback, this);
	state_estimate_pub_ = nh_.advertise<std_msgs::String>(state_estimate_topic,1);
}

void EKFEstimator::jointEncoderCallback(const sensor_msgs::JointState::ConstPtr& msg) {
	
}

void EKFEstimator::imuCallback(const sensor_msgs::Imu::ConstPtr& msg) {

}


