/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <ros/ros.h>

#include <geometry_msgs/Transform.h>
#include <sensor_msgs/JointState.h>
#include <nav_msgs/Odometry.h>

#include <mantis_state/state_client.h>
#include <mantis_params/param_client.h>
#include <mantis_kinematics/solver.h>

#include <tf2_ros/transform_listener.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/static_transform_broadcaster.h>

#include <eigen3/Eigen/Dense>

#include <string>
#include <vector>

class ForwardKinematics {
	private:
		ros::NodeHandle nh_;
		ros::NodeHandle nhp_;
		ros::Publisher pub_end_;
		ros::Publisher pub_viz_;
		ros::Timer timer_;
		ros::Timer timer_prop_viz_;

		tf2_ros::Buffer tfBuffer_;
		tf2_ros::TransformBroadcaster tfbr_;
		tf2_ros::StaticTransformBroadcaster tfsbr_;

		double param_rate_;
		bool param_do_end_effector_pose_;
		bool param_do_prop_viz_;

		MantisParams::Client p_;
		MantisState::Client s_;
		MantisSolver solver_;

		ros::Time time_last_update_;

		double prop_rate_;
		std::vector<geometry_msgs::TransformStamped> tf_props;

	public:
		ForwardKinematics( void );

		~ForwardKinematics( void );

		void do_reset();
		void check_update_time();

		void configure_static_joints();

		void callback_timer(const ros::TimerEvent& e);
		void callback_props(const ros::TimerEvent& e);
};
