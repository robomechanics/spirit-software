#include "local_planner/local_planner.h"

namespace plt = matplotlibcpp;

Eigen::IOFormat CleanFmt(4, 0, ", ", "\n", "[", "]");

LocalPlanner::LocalPlanner(ros::NodeHandle nh) :
  local_body_planner_(), local_footstep_planner_() {

	nh_ = nh;

    // Load rosparams from parameter server
  std::string terrain_map_topic, body_plan_topic, robot_state_topic, local_plan_topic,
    foot_plan_discrete_topic, foot_plan_continuous_topic;
  spirit_utils::loadROSParam(nh_, "topics/terrain_map", terrain_map_topic);
  spirit_utils::loadROSParam(nh_, "topics/body_plan", body_plan_topic);
  spirit_utils::loadROSParam(nh_, "topics/state/ground_truth",robot_state_topic);
  spirit_utils::loadROSParam(nh_, "topics/local_plan", local_plan_topic);
  spirit_utils::loadROSParam(nh_, "topics/foot_plan_discrete", foot_plan_discrete_topic);
  spirit_utils::loadROSParam(nh_, "topics/foot_plan_continuous", foot_plan_continuous_topic);
  spirit_utils::loadROSParam(nh_, "map_frame", map_frame_);

  // Setup pubs and subs
  terrain_map_sub_ = nh_.subscribe(terrain_map_topic,1, &LocalPlanner::terrainMapCallback, this);
  body_plan_sub_ = nh_.subscribe(body_plan_topic,1, &LocalPlanner::bodyPlanCallback, this);
  robot_state_sub_ = nh_.subscribe(robot_state_topic,1,&LocalPlanner::robotStateCallback,this);
  local_plan_pub_ = nh_.advertise<spirit_msgs::LocalPlan>(local_plan_topic,1);
  foot_plan_discrete_pub_ = nh_.advertise<
    spirit_msgs::MultiFootPlanDiscrete>(foot_plan_discrete_topic,1);
  foot_plan_continuous_pub_ = nh_.advertise<
    spirit_msgs::MultiFootPlanContinuous>(foot_plan_continuous_topic,1);


  // Load system parameters
  spirit_utils::loadROSParam(nh_, "local_planner/update_rate", update_rate_);
  spirit_utils::loadROSParam(nh_, "local_planner/timestep",dt_);
  spirit_utils::loadROSParam(nh_, "local_planner/iterations",iterations_);

  // Initialize nominal footstep positions projected down from the hips
  Eigen::Vector3d nominal_joint_state;
  nominal_joint_state << 0, 0.78, 1.57; // Default stand angles

  for (int i = 0; i < N_; ++i) {
    for (int j = 0; j < num_legs_; ++j) {
      Eigen::Vector3d toe_body_pos;
      kinematics_->bodyToFootFK(j, nominal_joint_state, toe_body_pos);
      hip_projected_foot_positions_.block<1,3>(i,j*3) = toe_body_pos;
    }
  }

  // Convert kinematics
  kinematics_ = std::make_shared<spirit_utils::SpiritKinematics>();

  initLocalBodyPlanner();
  initLocalFootstepPlanner();

  ROS_INFO("LocalPlanner setup complete, waiting for callbacks");
}

void LocalPlanner::initLocalBodyPlanner() {

    // Load MPC parameters 
  double m,Ixx,Iyy,Izz,mu,normal_lo, normal_hi;
  spirit_utils::loadROSParam(nh_, "local_body_planner/body_mass",m);
  spirit_utils::loadROSParam(nh_, "local_body_planner/body_ixx",Ixx);
  spirit_utils::loadROSParam(nh_, "local_body_planner/body_iyy",Iyy);
  spirit_utils::loadROSParam(nh_, "local_body_planner/body_izz",Izz);
  spirit_utils::loadROSParam(nh_, "local_body_planner/friction_mu",mu);
  spirit_utils::loadROSParam(nh_, "local_body_planner/normal_lo",normal_lo);
  spirit_utils::loadROSParam(nh_, "local_body_planner/normal_hi",normal_hi);

  std::vector<double> state_weights, control_weights, state_lower_bound, state_upper_bound;
  spirit_utils::loadROSParam(nh_, "local_body_planner/state_weights",state_weights);
  spirit_utils::loadROSParam(nh_, "local_body_planner/control_weights",control_weights);
  spirit_utils::loadROSParam(nh_, "local_body_planner/state_lower_bound",state_lower_bound);
  spirit_utils::loadROSParam(nh_, "local_body_planner/state_upper_bound",state_upper_bound);

  // Load state weights and bounds
  Eigen::MatrixXd Qx = Eigen::MatrixXd::Zero(Nx_, Nx_);
  Eigen::VectorXd state_lo = Eigen::VectorXd::Zero(Nx_);
  Eigen::VectorXd state_hi = Eigen::VectorXd::Zero(Nx_);
  for (int i = 0; i < Nx_; ++i) {
    Qx(i,i) = state_weights.at(i);
    state_lo(i) = state_lower_bound.at(i);
    state_hi(i) = state_upper_bound.at(i);
  }

  // Load control weights
  Eigen::MatrixXd Ru = Eigen::MatrixXd::Zero(Nu_,Nu_);
  for (int i = 0; i < 3; ++i) { // for each dimension
    for (int j = 0; j < 4; ++j) { //for each leg
      Ru(4*i + j,4*i+j) = control_weights.at(i);
    }
  }
  Ru(Nu_-1,Nu_-1) = 1e-6; //gravity weight term

  std::vector<Eigen::MatrixXd> Q_vec(N_+1);
  std::vector<Eigen::MatrixXd> U_vec(N_);
  for (int i = 0; i < N_+1; ++i) {
    Q_vec.at(i) = Qx;
  }

  for (int i = 0; i < N_; ++i) {
    U_vec.at(i) = Ru;
  }

  // Robot body inertia matrix
  Eigen::Matrix3d Ib = Eigen::Matrix3d::Zero();
  Ib.diagonal() << Ixx,Iyy,Izz;

  // Create convex mpc wrapper class
  local_body_planner_ = std::make_shared<QuadrupedMPC>();
  local_body_planner_->setTimestep(dt_);
  local_body_planner_->setMassProperties(m,Ib);
  local_body_planner_->update_weights(Q_vec,U_vec);
  local_body_planner_->update_state_bounds(state_lo, state_hi);
  local_body_planner_->update_control_bounds(normal_lo, normal_hi);
  local_body_planner_->update_friction(mu);

  // Setup pubs and subs
  ROS_INFO("Local Body Planner setup, waiting for callbacks");

}

void LocalPlanner::initLocalFootstepPlanner() {

  // Load parameters from server
  double grf_weight, max_footstep_horizon, period, ground_clearance;
  int num_cycles;
  spirit_utils::loadROSParam("local_footstep_planner/grf_weight", grf_weight);
  spirit_utils::loadROSParam("local_footstep_planner/max_footstep_horizon", max_footstep_horizon);
  spirit_utils::loadROSParam("local_footstep_planner/num_cycles", num_cycles);
  spirit_utils::loadROSParam("local_footstep_planner/period", period);
  spirit_utils::loadROSParam("local_footstep_planner/ground_clearance", ground_clearance);

  // Confirm grf weight is valid
  if (grf_weight>1 || grf_weight<0) {
    grf_weight = std::min(std::max(grf_weight,0.0),1.0);
    ROS_WARN("Invalid grf weight, clamping to %4.2f", grf_weight);
  }

  // Create footstep class
  local_footstep_planner_ = std::make_shared<LocalFootstepPlanner>();
  local_footstep_planner->setTemporalParams(period, num_cycles, max_footstep_horizon);
  local_footstep_planner->setSpatialParams(ground_clearance, grf_weight);
}

void LocalPlanner::terrainMapCallback(
  const grid_map_msgs::GridMap::ConstPtr& msg) {
  // Get the map in its native form
  grid_map::GridMap map;
  grid_map::GridMapRosConverter::fromMessage(*msg, map);

  // Convert to FastTerrainMap structure for faster querying
  terrain_.loadDataFromGridMap(map);
}

void LocalPlanner::globalPlanCallback(const spirit_msgs::BodyPlan::ConstPtr& msg) {
  global_plan_msg_ = msg;
}

void LocalPlanner::robotStateCallback(const spirit_msgs::RobotState::ConstPtr& msg) {

  if (msg->feet.feet.empty())
    return;

  if (body_plan_msg_ == NULL)
    return;

  current_state_ = spirit_utils::stateMsgToEigen(*msg);
}

void LocalPlanner::computeLocalPlan() {

  // // Initialize continuous foot plan with hip projected locations (constant)
  // foot_positions_ = hip_projected_foot_positions_;

  // // Iteratively generate body and footstep plans (non-parallelizable)
  // for (int i = 0; i < iterations_; i++) {
    
  //   // Compute body plan with MPC
  //   local_body_planner_.computePlan(current_state_, global_plan_msg_, foot_positions_,
  //     contact_sequences_, body_plan_, grf_plan_);
    
  //   // Compute the new footholds to match that body plan
  //   local_footstep_planner_.computeDiscretePlan(body_plan_, grf_plan_, foot_plan_discrete_msg_);

  //   // Convert the footholds to a FootTraj representation for body planning
  //   local_footstep_planner_.convertDiscretePlanToEigen(foot_plan_discrete_msg_, foot_positions_,
  //     contact_sequences_);
    
  // }

  // // Compute the continuous-time foot plan msg for publishing
  // local_footstep_planner_.computeContinuousPlan(body_plan_, foot_plan_discrete_msg_,
  //   foot_plan_continuous_msg_); 

}

void LocalPlanner::publishLocalPlan() {

  // // Initialize local plan message, set timestamp to now
  // spirit_msgs::LocalPlan local_plan_msg;
  // local_plan_msg.header.stamp = ros::Time::now();
  // local_plan_msg.header.frame = map_frame_;

  // // Add body, foot, joint, and grf data to the local plan message
  // for (int i = 0; i < body_plan_; i++) {

  //   ros::Time timestamp = local_plan_msg.header.stamp + ros::Duration(i*dt_);

  //   spirit_msgs::RobotState robot_state_msg;
  //   robot_state_msg.header.stamp = timestamp;
  //   robot_state_msg.header.frame = map_frame_;

  //   robot_state_msg.body = eigenToOdomMsg(body_plan_.row(i));
  //   robot_state_msg.feet = foot_plan_continuous_msg_[i];
  //   math_utils::ikRobotState(robot_state_msg);

  //   spirit_msgs::GRFArray grf_array_msg;
  //   grf_array_msg.header.stamp = timestamp;
  //   grf_array_msg.header.frame = map_frame_;

  //   grf_array_msg = eigenToGRFArrayMsg(grf_plan_.row(i), foot_plan_continuous_msg_[i]);
    

  //   local_plan_msg.states.push_back(robot_state_msg);
  //   local_plan_msg.grfs.push_back(grf_array_msg);
  // }

  // // Publish
  // local_plan_pub_.publish(local_plan_msg);
  // foot_plan_discrete_pub_.publish(foot_plan_discrete_msg_);
}

void LocalPlanner::spin() {

  ros::Rate r(compute_rate_);

  while (ros::ok()) {

    ros::spinOnce();
    
    // Publish local plan data (state reference traj and desired GRF array)
    computeLocalPlan();
    publishLocalPlan();
    
    r.sleep();
  }
}