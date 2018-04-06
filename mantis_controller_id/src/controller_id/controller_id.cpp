#include <ros/ros.h>

#include <controller_id/controller_id.h>
#include <controller_id/controller_id_params.h>
#include <pidController/pidController.h>
#include <controller_id/se_tools.h>
#include <dynamics/calc_Dq.h>
#include <dynamics/calc_Cqqd.h>
#include <dynamics/calc_Lqd.h>
#include <dynamics/calc_Jj2.h>
#include <dynamics/calc_Je.h>
#include <dh_parameters/dh_parameters.h>
#include <mantis_paths/path_extract.h>

#include <mavros_msgs/OverrideRCIn.h>
#include <sensor_msgs/JointState.h>
#include <sensor_msgs/Imu.h>

#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/TwistStamped.h>
#include <geometry_msgs/AccelStamped.h>
#include <geometry_msgs/Vector3.h>
#include <geometry_msgs/Quaternion.h>

#include <eigen3/Eigen/Dense>
#include <math.h>
#include <iostream>

#define CONST_GRAV 9.80665

ControllerID::ControllerID() :
	nh_(),
	nhp_("~"),
	p_(&nh_, &nhp_),
	ref_path_(&nhp_),
	param_frame_id_("map"),
	param_model_id_("mantis_uav"),
	param_use_imu_state_(false),
	param_wait_for_path_(false),
	param_track_base_(false),
	param_track_j2_(false),
	param_accurate_z_tracking_(false),
	param_accurate_end_tracking_(false),
	param_reference_feedback_(false),
	param_rate_(100) {

	bool success = true;

	nhp_.param("frame_id", param_frame_id_, param_frame_id_);
	nhp_.param("model_id", param_model_id_, param_model_id_);
	nhp_.param("wait_for_path", param_wait_for_path_, param_wait_for_path_);
	nhp_.param("use_imu_state", param_use_imu_state_, param_use_imu_state_);
	nhp_.param("track_base", param_track_base_, param_track_base_);
	nhp_.param("track_j2", param_track_j2_, param_track_j2_);
	nhp_.param("accurate_z_tracking", param_accurate_z_tracking_, param_accurate_z_tracking_);
	nhp_.param("accurate_end_tracking", param_accurate_end_tracking_, param_accurate_end_tracking_);
	nhp_.param("reference_feedback", param_reference_feedback_, param_reference_feedback_);
	nhp_.param("control_rate", param_rate_, param_rate_);

	//Load the robot parameters
	p_.load();	//TODO have this give a success if loaded correctly

	//Load in the joint definitions
	for(int i=0; i<p_.link_num; i++) {
		DHParameters dh( &nh_, "links/l" + std::to_string(i) );

		if( dh.is_valid() ) {
			joints_.push_back(dh);
		} else {
			ROS_FATAL("Error loading joint %i", i);
			success = false;
			break;
		}
	}

	pos_pid_x_.setGains( p_.gain_position_xy_p, p_.gain_position_xy_i, 0.0, 0.0 );
	pos_pid_y_.setGains( p_.gain_position_xy_p, p_.gain_position_xy_i, 0.0, 0.0 );
	pos_pid_z_.setGains( p_.gain_position_z_p, p_.gain_position_z_i, 0.0, 0.0 );
	pos_pid_x_.setOutputMinMax( -CONST_GRAV / 2.0, CONST_GRAV / 2.0);
	pos_pid_y_.setOutputMinMax( -CONST_GRAV / 2.0, CONST_GRAV / 2.0);
	pos_pid_z_.setOutputMinMax( -CONST_GRAV / 2.0, CONST_GRAV / 2.0);

	if(success) {
		ROS_INFO( "Loaded configuration for %li links", joints_.size() );

		pub_rc_ = nhp_.advertise<mavros_msgs::OverrideRCIn>("output/rc", 10);
		pub_joints_ = nhp_.advertise<sensor_msgs::JointState>("output/joints", 10);

		pub_pose_base_ = nhp_.advertise<geometry_msgs::PoseStamped>("feedback/pose/base", 10);
		pub_pose_end_ = nhp_.advertise<geometry_msgs::PoseStamped>("feedback/pose/end_effector", 10);
		pub_twist_base_ = nhp_.advertise<geometry_msgs::TwistStamped>("feedback/twist/end_effector", 10);
		pub_twist_end_ = nhp_.advertise<geometry_msgs::TwistStamped>("feedback/twist/base", 10);
		pub_accel_linear_ = nhp_.advertise<geometry_msgs::AccelStamped>("feedback/accel/linear", 10);
		pub_accel_body_ = nhp_.advertise<geometry_msgs::AccelStamped>("feedback/accel/body", 10);

		sub_state_odom_ = nhp_.subscribe<nav_msgs::Odometry>( "state/odom", 10, &ControllerID::callback_state_odom, this );
		sub_state_imu_ = nhp_.subscribe<sensor_msgs::Imu>( "state/imu", 10, &ControllerID::callback_state_imu, this );
		sub_state_joints_ = nhp_.subscribe<sensor_msgs::JointState>( "state/joints", 10, &ControllerID::callback_state_joints, this );

		//XXX: Initialize takeoff goals
		ref_path_.set_latest( Eigen::Vector3d(p_.takeoff_x, p_.takeoff_y, p_.takeoff_z), Eigen::Quaterniond::Identity() );

		ROS_INFO("Inverse dynamics controller loaded. Waiting for inputs");

		//Lock the controller until all the inputs are satisfied
		while( ( !ref_path_.received_valid_path() && param_wait_for_path_ ) ||
			   ( msg_state_odom_.header.stamp == ros::Time(0) ) ||
			   ( msg_state_joints_.header.stamp == ros::Time(0) ) ||
			   ( param_use_imu_state_ && ( msg_state_imu_.header.stamp == ros::Time(0) ) ) ) {
			if( !ros::ok() )
				break;

			 ros::spinOnce();
			 ros::Rate(param_rate_).sleep();
		}

		//Start the control loop
		timer_ = nhp_.createTimer(ros::Duration(1.0/param_rate_), &ControllerID::callback_control, this );

		ROS_INFO("Inverse dynamics controller started!");
	} else {
		ROS_WARN("Inverse Dynamics controller shutting down.");
		ros::shutdown();
	}
}

ControllerID::~ControllerID() {
}

void ControllerID::callback_control(const ros::TimerEvent& e) {
	std::vector<uint16_t> pwm_out(p_.motor_num);	//Allocate space for the number of motors
	std::vector<double> joints_out(p_.manip_num);

	double dt = (e.current_real - e.last_real).toSec();

	//If we still have all the inputs satisfied
	if( ( ref_path_.received_valid_path() || !param_wait_for_path_ ) &&
		( msg_state_odom_.header.stamp != ros::Time(0) ) &&
		( msg_state_joints_.header.stamp != ros::Time(0) ) &&
		( ( !param_use_imu_state_ ) || ( msg_state_imu_.header.stamp != ros::Time(0) ) ) ) {

		double num_states = 6 + p_.manip_num;	//XXX: 6 comes from XYZ + Wrpy

		//Current States
		Eigen::Affine3d g = affine_from_msg(msg_state_odom_.pose.pose);

		Eigen::Vector3d bv(msg_state_odom_.twist.twist.linear.x,
						   msg_state_odom_.twist.twist.linear.y,
						   msg_state_odom_.twist.twist.linear.z);

		Eigen::Vector3d bw;
		if( param_use_imu_state_ ) {
			bw = Eigen::Vector3d(msg_state_imu_.angular_velocity.x,
								 msg_state_imu_.angular_velocity.y,
								 msg_state_imu_.angular_velocity.z);
		} else {
			bw = Eigen::Vector3d(msg_state_odom_.twist.twist.angular.x,
								 msg_state_odom_.twist.twist.angular.y,
								 msg_state_odom_.twist.twist.angular.z);
		}

		Eigen::VectorXd r = Eigen::VectorXd::Zero(p_.manip_num);
		Eigen::VectorXd rd = Eigen::VectorXd::Zero(p_.manip_num);
		Eigen::VectorXd rdd = Eigen::VectorXd::Zero(p_.manip_num);

		int jc = 0;
		for(int i=0; i<joints_.size(); i++) {
			//Only update the dynamic links
			if(joints_[i].jt() != DHParameters::JointType::Static) {
				r(jc) = joints_[i].q();
				rd(jc) = joints_[i].qd();
				rdd(jc) = joints_[i].qdd();

				jc++;
			}
		}

		Eigen::VectorXd qd = Eigen::VectorXd::Zero(num_states);
		qd << bv, bw, rd;	//Full body-frame velocity state vector

		//Goal States
		Eigen::Affine3d ge_sp = Eigen::Affine3d::Identity();
		Eigen::Vector3d gev_sp = Eigen::Vector3d::Zero();
		Eigen::Affine3d g_sp = Eigen::Affine3d::Identity();
		Eigen::Vector3d gv_sp = Eigen::Vector3d::Zero();

		if( !ref_path_.get_ref_state(ge_sp, gev_sp, e.current_real) ) {
			//There was an issue setting the path goal
			//Hold latest position

			//XXX: This isn't needed as it is done internally
			//ge_sp = latest_g_sp_;
			//gev_sp = Eigen::Vector3d::Zero();
		}

		if(param_track_base_) {
			g_sp = ge_sp;
			gv_sp = gev_sp;
		} else {
			//Compute transform for all joints in the chain to get base to end effector
			Eigen::Affine3d gbe = Eigen::Affine3d::Identity();
			Eigen::MatrixXd Je = Eigen::MatrixXd::Zero(6, rd.size());

			//This is only really here for the paper test case
			//This hole thing could be done dynamically for any joint
			if(param_track_j2_) {
				//Compute transform for all joints in the chain to get base to Joint 2
				for(int i=0; i<2; i++) {
					gbe = gbe * joints_[i].transform();
				}

				calc_Jj2(Je, joints_[1].r());

			} else {
				//Compute transform for all joints in the chain to get base to end effector
				for(int i=0; i<joints_.size(); i++) {
					gbe = gbe * joints_[i].transform();
				}

				//The linear velocity of the base is dependent on the joint velocities the end effector
				//End effector velocity jacobian
				//XXX: Pretty sure this can be done dynamically in dh_parameters
				calc_Je(Je,
						joints_[1].r(),
						joints_[2].r(),
						joints_[2].q());
			}

			//Calculate the tracking offset for the base
			g_sp = calc_goal_base_transform(ge_sp, gbe);

			if(param_accurate_z_tracking_) {
				//Get the current end effector pose
				Eigen::Affine3d ge_c = g*gbe;
				//Translate it to the desired setpoint
				ge_c.translation() = ge_sp.translation();
				//Get the base pose based off of the translated setpoint
				Eigen::Affine3d g_sp_c = ge_c*gbe.inverse();

				double z_corr = g_sp_c.translation().z() - g_sp.translation().z();

				ROS_INFO_STREAM("z_corr: " << z_corr);
				//Override the calculated z translation with one that matches the current pose
				g_sp.translation().z() += z_corr;
			}

			//Compensate for velocity terms in manipulator movement
			//This is more accurate, but can make things more unstable
			if(param_accurate_end_tracking_) {
				gv_sp = calc_goal_base_velocity(gev_sp, g.linear()*gbe.linear(), Je, rd);
			} else {
				gv_sp = gev_sp;
			}
		}

		//Trajectory Tracking Controller
		pos_pid_x_.setGains( p_.gain_position_xy_p, p_.gain_position_xy_i, 0.0, 0.0 );
		pos_pid_y_.setGains( p_.gain_position_xy_p, p_.gain_position_xy_i, 0.0, 0.0 );
		pos_pid_z_.setGains( p_.gain_position_z_p, p_.gain_position_z_i, 0.0, 0.0 );

		Eigen::Vector3d bv_world = g.linear()*bv;
		double a_p_x = pos_pid_x_.step(dt, g_sp.translation().x(), g.translation().x(), bv_world.x());
		double a_p_y = pos_pid_y_.step(dt, g_sp.translation().y(), g.translation().y(), bv_world.y());
		double a_p_z = pos_pid_z_.step(dt, g_sp.translation().z(), g.translation().z(), bv_world.z());

		Eigen::Vector3d e_p_v = gv_sp - bv_world;
		double a_v_x = p_.gain_velocity_xy_p * e_p_v.x();
		double a_v_y = p_.gain_velocity_xy_p * e_p_v.y();
		double a_v_z = p_.gain_velocity_z_p * e_p_v.z();

		Eigen::Vector3d Al(a_p_x + a_v_x, a_p_y + a_v_y, a_p_z + a_v_z);

		//Add in gravity compensation
		Eigen::Vector3d A = Al + Eigen::Vector3d(0.0, 0.0, CONST_GRAV);

		A(2) = (A(2) < 0.1) ? 0.1 : A(2);	//Can't accelerate downward faster than -g (take a little)
		//XXX:
		//	A bit dirty, but this will stop acceleration going less than
		//	-g and will keep the gr_sp from rotating by more than pi/2
		Eigen::Vector3d Axy(A.x(), A.y(), 0.0);
		if(Axy.norm() > A.z()) {
			double axy_scale = Axy.norm() / A.z();
			Axy = Axy / axy_scale;
			A.segment(0,2) << Axy.segment(0,2);
		}
		//TODO: Constrain vertical max accel(?)

		//Calculate the corresponding rotation to reach desired acceleration
		//We generate at it base on now yaw, then rotate it to reach the desired orientation
		Eigen::Vector3d An = A.normalized();
		Eigen::Vector3d y_c = g_sp.linear().col(1).normalized();
		Eigen::Vector3d r_sp_x = y_c.cross(An).normalized();
		Eigen::Vector3d r_sp_y = An.cross(r_sp_x).normalized();
		Eigen::Matrix3d gr_sp;
		gr_sp << r_sp_x, r_sp_y, An;

		//Calculate the goal rates to achieve the right acceleration vector
		//Eigen::VectorXd e_w = vector_interlace(calc_ang_error(gr_sp, g.linear()), Eigen::Vector3d::Zero() - bw);
		//Eigen::Vector3d wa = Kw*e_w;
		Eigen::Vector3d w_goal = calc_ang_error(gr_sp, g.linear());
		Eigen::Vector3d wa = p_.gain_rotation_rate_p*(w_goal - bw);

		//Calculate the required manipulator accelerations
		//Eigen::VectorXd e_r = vector_interlace(r_sp - r, Eigen::VectorXd::Zero(p_.manip_num) - rd);
		//Eigen::VectorXd ra = Kr*e_r;

		//Calculate Abz such that it doesn't apply too much thrust until fully rotated
		Eigen::Vector3d body_z = g.linear()*Eigen::Vector3d::UnitZ();
		double Az_scale = A.z() / body_z.z();
		Eigen::Vector3d Abz_accel = Az_scale*body_z;

		Eigen::VectorXd ua = Eigen::VectorXd::Zero(num_states);	//vd(6,1) + rdd(2,1)
		ua(0) = 0.0;
		ua(1) = 0.0;
		ua(2) = Abz_accel.norm();
		//ua(2) = A.norm();
		ua.segment(3,3) << wa;
		ua.segment(6,p_.manip_num) << rdd;

		//ROS_INFO_STREAM("Error: " << A.z() - Abz_accel.z() << std::endl << "Linear: " << A.norm() << std::endl << "Body: " << Abz_accel.norm());

		//Calculate dynamics matricies
		Eigen::MatrixXd D = Eigen::MatrixXd::Zero(num_states, num_states);
		Eigen::MatrixXd C = Eigen::MatrixXd::Zero(num_states, num_states);
		Eigen::MatrixXd L = Eigen::MatrixXd::Zero(num_states, num_states);

		//XXX: Take care!
		double l0 = joints_[0].d();
		double l1 = joints_[1].r();

		calc_Dq(D,
				p_.I0x, p_.I0y, p_.I0z,
				p_.I1x, p_.I1y, p_.I1z,
				p_.I2x, p_.I2y, p_.I2z,
				l0, l1, p_.lc1, p_.lc2,
				p_.m0, p_.m1, p_.m2,
				r(0), r(1));

		calc_Cqqd(C,
				  p_.I1x, p_.I1y,
				  p_.I2x, p_.I2y,
				  bv(0), bv(1), bv(2), bw(0), bw(1), bw(2),
				  l0, l1, p_.lc1, p_.lc2,
				  p_.m1, p_.m2,
				  r(0), rd(0), r(1), rd(1));

		Eigen::VectorXd tau = D*ua + (C + L)*qd;

		//XXX: Hardcoded for hex in function because lazy
		Eigen::MatrixXd M = Eigen::MatrixXd::Zero(p_.motor_num + p_.manip_num, num_states);
		calc_motor_map(M);

		Eigen::MatrixXd u = M*tau;

		//Calculate goal PWM values to generate desired torque
		for(int i=0; i<p_.motor_num; i++) {
			pwm_out[i] = map_pwm(u(i));
		}

		for(int i=0; i<p_.manip_num; i++) {
			joints_out[i] = u(p_.motor_num + i);
		}

		if(param_reference_feedback_)
			message_output_feedback(e.current_real, g_sp, ge_sp, gv_sp, gev_sp, Al, gr_sp, ua);
	} else {
		ROS_ERROR_THROTTLE(0.5, "LOST INPUT!!");

		//Output minimums until the input info is available
		for(int i=0; i<p_.motor_num; i++) {
			pwm_out[i] = p_.pwm_min;
		}

		for(int i=0; i<p_.manip_num; i++) {
			joints_out[i] = 0.0;
		}

		//Reset the PIDs and keep the integrator down
		pos_pid_x_.reset(msg_state_odom_.pose.pose.position.x);
		pos_pid_y_.reset(msg_state_odom_.pose.pose.position.y);
		pos_pid_z_.reset(msg_state_odom_.pose.pose.position.z);
	}

	message_output_control(e.current_real, pwm_out, joints_out);
}

void ControllerID::matrix_clamp(Eigen::MatrixXd m, const double min, const double max) {
	for(int i=0; i<m.rows(); i++) {
		for(int j=0; j<m.cols(); j++) {
			m(i,j) = (m(i,j) < min) ? min : (m(i,j) > max) ? max : m(i,j);
		}
	}
}

Eigen::VectorXd ControllerID::vector_interlace(const Eigen::VectorXd a, const Eigen::VectorXd b) {
	ROS_ASSERT_MSG(a.size() == b.size(), "Vectors to be interlaced must be same size (a=%li,b=%li", a.size(), b.size());

	Eigen::VectorXd c = Eigen::VectorXd::Zero(2*a.size());

	for(int i=0; i<a.size(); i++) {
		c.segment(2*i,2) << a(i), b(i);
	}

	return c;
}

Eigen::Vector3d ControllerID::position_from_msg(const geometry_msgs::Point p) {
		return Eigen::Vector3d(p.x, p.y, p.z);
}

Eigen::Quaterniond ControllerID::quaternion_from_msg(const geometry_msgs::Quaternion q) {
		return Eigen::Quaterniond(q.w, q.x, q.y, q.z).normalized();
}

Eigen::Affine3d ControllerID::affine_from_msg(const geometry_msgs::Pose pose) {
		Eigen::Affine3d a;

		a.translation() << position_from_msg(pose.position);
		a.linear() << quaternion_from_msg(pose.orientation).toRotationMatrix();

		return a;
}

Eigen::Affine3d ControllerID::calc_goal_base_transform(const Eigen::Affine3d &ge_sp, const Eigen::Affine3d &gbe) {
	//Use this yaw only rotation to set the direction of the base (and thus end effector)
	Eigen::Matrix3d br_sp = extract_yaw_component(ge_sp.linear());

	//Construct the true end effector
	Eigen::Affine3d sp = Eigen::Affine3d::Identity();
	sp.linear() = br_sp*gbe.linear();
	sp.translation() = ge_sp.translation();

	//Use the inverse transform to get the true base transform
	return sp*gbe.inverse();
}

Eigen::Vector3d ControllerID::calc_goal_base_velocity(const Eigen::Vector3d &gev_sp, const Eigen::Matrix3d &Re, const Eigen::MatrixXd &Je, const Eigen::VectorXd &rd) {
	//Velocity of the end effector in the end effector frame
	Eigen::VectorXd vbe = Je*rd;

	//Eigen::Affine3d Vbe;
	//Vbe.translation() << vbe.segment(0,3);
	//Vbe.linear() << vee_up(vbe.segment(3,3));

	//Get linear velocity in the world frame
	Eigen::Vector3d Ve = Re*vbe.segment(0,3);

	//Only care about the translation movements
	return gev_sp - Ve;
}

Eigen::Matrix3d ControllerID::extract_yaw_component(const Eigen::Matrix3d r) {
	Eigen::Vector3d sp_x = Eigen::Vector3d::UnitX();
	Eigen::Vector3d sp_y = Eigen::Vector3d::UnitY();

	//As long as y isn't straight up
	if(r.col(1) != Eigen::Vector3d::UnitZ()) {
		//If we have ||roll|| > 90Deg
		Eigen::Vector3d y_c;
		if(r(2,2) > 0.0) {
			y_c = r.col(1);
		} else {
			y_c = -r.col(1);
		}

		sp_x = y_c.cross(Eigen::Vector3d::UnitZ());
		sp_y = Eigen::Vector3d::UnitZ().cross(sp_x);
	} else { //Use X-axis for the edge-case
		Eigen::Vector3d x_c = r.col(0);

		sp_y = Eigen::Vector3d::UnitZ().cross(x_c);
		sp_x = sp_y.cross(Eigen::Vector3d::UnitZ());
	}

	//Get the pure yaw rotation
	Eigen::Matrix3d ry;
	ry << sp_x.normalized(),
		  sp_y.normalized(),
		  Eigen::Vector3d::UnitZ();

	return ry;
}

Eigen::Vector3d ControllerID::calc_ang_error(const Eigen::Matrix3d &R_sp, const Eigen::Matrix3d &R) {
	Eigen::Matrix3d I = Eigen::Matrix3d::Identity();

	//Method derived from px4 attitude controller:
	//DCM from for state and setpoint

	//Calculate shortest path to goal rotation without yaw (as it's slower than roll/pitch)
	Eigen::Vector3d R_z = R.col(2);
	Eigen::Vector3d R_sp_z = R_sp.col(2);

	//px4: axis and sin(angle) of desired rotation
	//px4: math::Vector<3> e_R = R.transposed() * (R_z % R_sp_z);
	Eigen::Vector3d e_R = R.transpose() * R_z.cross(R_sp_z);

	double e_R_z_sin = e_R.norm();
	double e_R_z_cos = R_z.dot(R_sp_z);

	//px4: calculate rotation matrix after roll/pitch only rotation
	Eigen::Matrix3d R_rp;

	if(e_R_z_sin > 0) {
		//px4: get axis-angle representation
		Eigen::Vector3d e_R_z_axis = e_R / e_R_z_sin;
		e_R = e_R_z_axis * std::atan2(e_R_z_sin, e_R_z_cos);

		//px4: cross product matrix for e_R_axis
		Eigen::Matrix3d e_R_cp;
		e_R_cp(0,1) = -e_R_z_axis.z();
		e_R_cp(0,2) = e_R_z_axis.y();
		e_R_cp(1,0) = e_R_z_axis.z();
		e_R_cp(1,2) = -e_R_z_axis.x();
		e_R_cp(2,0) = -e_R_z_axis.y();
		e_R_cp(2,1) = e_R_z_axis.x();

		//px4: rotation matrix for roll/pitch only rotation
		R_rp = R * ( I + (e_R_cp * e_R_z_sin) + ( (e_R_cp * e_R_cp) * (1.0 - e_R_z_cos) ) );
	} else {
		//px4: zero roll/pitch rotation
		R_rp = R;
	}

	//px4: R_rp and R_sp has the same Z axis, calculate yaw error
	Eigen::Vector3d R_sp_x = R_sp.col(0);
	Eigen::Vector3d R_rp_x = R_rp.col(0);

	//px4: calculate weight for yaw control
	double yaw_w = e_R_z_cos * e_R_z_cos;

	//px4: e_R(2) = atan2f((R_rp_x % R_sp_x) * R_sp_z, R_rp_x * R_sp_x) * yaw_w;
	//Eigen::Vector3d R_rp_c_sp = R_rp_x.cross(R_sp_x);
	//e_R(2) = std::atan2(R_rp_c_sp.dot(R_sp_z), R_rp_x.dot(R_sp_x)) * yaw_w;
	e_R(2) = std::atan2( (R_rp_x.cross(R_sp_x)).dot(R_sp_z), R_rp_x.dot(R_sp_x)) * yaw_w;

	if(e_R_z_cos < 0) {
		//px4: for large thrust vector rotations use another rotation method:
		//For normal operation, this implies a roll/pitch change greater than pi
		//Should never be an issue for us
		ROS_WARN_THROTTLE(1.0, "Large thrust vector detected!");
	}

	return p_.gain_rotation_ang_p*e_R;
}

int16_t ControllerID::map_pwm(double val) {
	//Constrain from 0 -> 1
	double c = (val > 1.0) ? 1.0 : (val < 0.0) ? 0.0 : val;

	//Scale c to the pwm values
	return int16_t((p_.pwm_max - p_.pwm_min)*c) + p_.pwm_min;
}
/*
void ControllerID::calc_motor_map(Eigen::MatrixXd &M) {
	//TODO: This all needs to be better defined generically
	double arm_ang = M_PI / 3.0;

	double kT = 1.0 / (p_.motor_num * p_.motor_thrust_max);
	double ktx = 1.0 / (2.0 * p_.la * (2.0 * std::sin(arm_ang / 2.0) + 1.0) * p_.motor_thrust_max);
	double kty = 1.0 / (4.0 * p_.la * std::cos(arm_ang  / 2.0) * p_.motor_thrust_max);
	double km = -1.0 / (p_.motor_num * p_.motor_drag_max);
	//km = 0;	//TODO: Need to pick motor_drag_max!!!

	//Generate the copter map
	Eigen::MatrixXd cm = Eigen::MatrixXd::Zero(p_.motor_num, 6);
	cm << 0.0, 0.0,  kT, -ktx,  0.0, -km,
		  0.0, 0.0,  kT,  ktx,  0.0,  km,
		  0.0, 0.0,  kT,  ktx, -kty, -km,
		  0.0, 0.0,  kT, -ktx,  kty,  km,
		  0.0, 0.0,  kT, -ktx, -kty,  km,
		  0.0, 0.0,  kT,  ktx,  kty, -km;

	M << cm, Eigen::MatrixXd::Zero(p_.motor_num, p_.manip_num),
		 Eigen::MatrixXd::Zero(p_.manip_num, 6), Eigen::MatrixXd::Identity(p_.manip_num, p_.manip_num);
}
*/
void ControllerID::calc_motor_map(Eigen::MatrixXd &M) {
	//TODO: This all needs to be better defined generically
	double arm_ang = M_PI / 3.0;

	double kT = 1.0 / (p_.motor_num * p_.motor_thrust_max);
	double ktx = 1.0 / (2.0 * p_.la * (2.0 * std::sin(arm_ang / 2.0) + 1.0) * p_.motor_thrust_max);
	double kty = 1.0 / (4.0 * p_.la * std::cos(arm_ang  / 2.0) * p_.motor_thrust_max);
	double km = -1.0 / (p_.motor_num * p_.motor_drag_max);

	//Generate the copter map
	Eigen::MatrixXd cm = Eigen::MatrixXd::Zero(p_.motor_num, 6);
	cm << 0.0, 0.0,  kT, -ktx,  0.0, -km,
		  0.0, 0.0,  kT,  ktx,  0.0,  km,
		  0.0, 0.0,  kT,  ktx, -kty, -km,
		  0.0, 0.0,  kT, -ktx,  kty,  km,
		  0.0, 0.0,  kT, -ktx, -kty,  km,
		  0.0, 0.0,  kT,  ktx,  kty, -km;

	M << cm, Eigen::MatrixXd::Zero(p_.motor_num, p_.manip_num),
		 Eigen::MatrixXd::Zero(p_.manip_num, 6), Eigen::MatrixXd::Identity(p_.manip_num, p_.manip_num);
}

void ControllerID::message_output_control(const ros::Time t, const std::vector<uint16_t> &pwm, const std::vector<double> &joints) {
	mavros_msgs::OverrideRCIn msg_rc_out;
	sensor_msgs::JointState msg_joints_out;

	//Prepare headers
	//msg_rc_out.header.stamp = t;
	//msg_rc_out.header.frame_id = param_frame_id_;
	msg_joints_out.header.stamp = t;
	msg_joints_out.header.frame_id = param_model_id_;

	//Insert control data
	ROS_ASSERT_MSG(pwm.size() <= 8, "Supported number of motors is 8 (%li)", pwm.size());
	for(int i=0; i<8; i++) {
		if( i<pwm.size() ) {
			msg_rc_out.channels[i] = pwm[i];
		} else {
			msg_rc_out.channels[i] = msg_rc_out.CHAN_NOCHANGE;
		}
	}

	msg_joints_out.name = msg_state_joints_.name;
	msg_joints_out.position = msg_state_joints_.position;
	msg_joints_out.effort = joints;

	//Publish messages
	pub_rc_.publish(msg_rc_out);
	pub_joints_.publish(msg_joints_out);
}

void ControllerID::message_output_feedback(const ros::Time t,
										   const Eigen::Affine3d &g_sp,
										   const Eigen::Affine3d &ge_sp,
										   const Eigen::Vector3d &gv_sp,
										   const Eigen::Vector3d &gev_sp,
										   const Eigen::Vector3d &pa,
										   const Eigen::Matrix3d &r_sp,
										   const Eigen::VectorXd &ua) {
	geometry_msgs::PoseStamped msg_pose_base_out;
	geometry_msgs::PoseStamped msg_pose_end_out;
	geometry_msgs::TwistStamped msg_twist_base_out;
	geometry_msgs::TwistStamped msg_twist_end_out;
	geometry_msgs::AccelStamped msg_accel_linear_out;
	geometry_msgs::AccelStamped msg_accel_body_out;

	//Prepare headers
	msg_pose_base_out.header.stamp = t;
	msg_pose_base_out.header.frame_id = param_frame_id_;
	msg_pose_end_out.header.stamp = t;
	msg_pose_end_out.header.frame_id = param_frame_id_;

	msg_twist_base_out.header.stamp = t;
	msg_twist_base_out.header.frame_id = param_frame_id_;
	msg_twist_end_out.header.stamp = t;
	msg_twist_end_out.header.frame_id = param_frame_id_;

	msg_accel_linear_out.header.stamp = t;
	msg_accel_linear_out.header.frame_id = param_frame_id_;
	msg_accel_body_out.header.stamp = t;
	msg_accel_body_out.header.frame_id = param_model_id_;

	//Insert feedback data
	Eigen::Quaterniond r_sp_q(r_sp);
	r_sp_q.normalize();
	msg_pose_base_out.pose.position.x = g_sp.translation().x();
	msg_pose_base_out.pose.position.y = g_sp.translation().y();
	msg_pose_base_out.pose.position.z = g_sp.translation().z();
	msg_pose_base_out.pose.orientation.w = r_sp_q.w();
	msg_pose_base_out.pose.orientation.x = r_sp_q.x();
	msg_pose_base_out.pose.orientation.y = r_sp_q.y();
	msg_pose_base_out.pose.orientation.z = r_sp_q.z();

	Eigen::Quaterniond ge_sp_q(ge_sp.linear());
	ge_sp_q.normalize();
	msg_pose_end_out.pose.position.x = ge_sp.translation().x();
	msg_pose_end_out.pose.position.y = ge_sp.translation().y();
	msg_pose_end_out.pose.position.z = ge_sp.translation().z();
	msg_pose_end_out.pose.orientation.w = ge_sp_q.w();
	msg_pose_end_out.pose.orientation.x = ge_sp_q.x();
	msg_pose_end_out.pose.orientation.y = ge_sp_q.y();
	msg_pose_end_out.pose.orientation.z = ge_sp_q.z();

	msg_twist_base_out.twist.linear.x = gv_sp.x();
	msg_twist_base_out.twist.linear.y = gv_sp.y();
	msg_twist_base_out.twist.linear.z = gv_sp.z();

	msg_twist_end_out.twist.linear.x = gev_sp.x();
	msg_twist_end_out.twist.linear.y = gev_sp.y();
	msg_twist_end_out.twist.linear.z = gev_sp.z();

	msg_accel_linear_out.accel.linear.x = pa.x();
	msg_accel_linear_out.accel.linear.y = pa.y();
	msg_accel_linear_out.accel.linear.z = pa.z();

	msg_accel_body_out.accel.linear.x = ua(0);
	msg_accel_body_out.accel.linear.y = ua(1);
	msg_accel_body_out.accel.linear.z = ua(2);
	msg_accel_body_out.accel.angular.x = ua(3);
	msg_accel_body_out.accel.angular.y = ua(4);
	msg_accel_body_out.accel.angular.z = ua(5);

	//Publish messages
	pub_pose_base_.publish(msg_pose_base_out);
	pub_pose_end_.publish(msg_pose_end_out);
	pub_twist_base_.publish(msg_twist_base_out);
	pub_twist_end_.publish(msg_twist_end_out);
	pub_accel_linear_.publish(msg_accel_linear_out);
	pub_accel_body_.publish(msg_accel_body_out);
}

void ControllerID::callback_state_odom(const nav_msgs::Odometry::ConstPtr& msg_in) {
	msg_state_odom_ = *msg_in;
}

void ControllerID::callback_state_imu(const sensor_msgs::Imu::ConstPtr& msg_in) {
	msg_state_imu_ = *msg_in;
}

void ControllerID::callback_state_joints(const sensor_msgs::JointState::ConstPtr& msg_in) {
	double dt = (msg_state_joints_.header.stamp - msg_in->header.stamp).toSec();

	for(int i=0; i<joints_.size(); i++) {
		//Only update the dynamic links
		if(joints_[i].jt() != DHParameters::JointType::Static) {
			for(int j=0; j<msg_in->name.size(); j++) {
				//If we have the right joint
				if(joints_[i].name() == msg_in->name[j])
					joints_[i].update(msg_in->position[j], msg_in->velocity[j], dt);
			}
		}
	}

	msg_state_joints_ = *msg_in;
}
