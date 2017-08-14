#include <ros/ros.h>

#include <tf2/LinearMath/Quaternion.h>
#include <tf2/LinearMath/Matrix3x3.h>

#include <mavros_msgs/AttitudeTarget.h>
#include <mavros_msgs/RCOut.h>

#include <geometry_msgs/Pose.h>
#include <geometry_msgs/Twist.h>
#include <nav_msgs/Odometry.h>

#include <pidController/pidController.h>

#include <string>

#define NUM_MOTORS 6

typedef struct motorMixer_s {
    double throttle;
    double roll;
    double pitch;
    double yaw;
} motorMixer_t;

typedef struct controlCommand_s {
	double T;
	double r;
	double p;
	double y;
} controlCommand_t;

const static motorMixer_t mixer_hexacopter_x[NUM_MOTORS] = {
	{ 1.0,	-1.0,	 0.0,		 1.0}, // Motor 1
	{ 1.0,	 1.0,	 0.0,		-1.0}, // Motor 2
	{ 1.0,	 0.5,	-0.866025,	 1.0}, // Motor 3
	{ 1.0,	-0.5,	 0.866025,	-1.0}, // Motor 4
	{ 1.0,	-0.5,	-0.866025,	-1.0}, // Motor 5
	{ 1.0,	 0.5,	 0.866025,	 1.0}, // Motor 6
};

class ControllerAcro {
	private:
		ros::NodeHandle nh_;

		ros::Subscriber sub_attitude_target_;
		ros::Subscriber sub_odom_;
		ros::Publisher pub_rc_out_;

		std::string topic_attitude_target_;
		std::string topic_odom_;
		std::string topic_rc_out_;

		std::string model_name_;
		geometry_msgs::Pose model_current_pose_;
		geometry_msgs::Twist model_current_twist_;


		ros::Timer tmr_rc_out_;
		double pwm_update_rate_;
		std::string frame_id_;
		mavros_msgs::RCOut msg_rc_out_;

		mavros_msgs::AttitudeTarget goal_att_;

		double param_ang_r_ff_;
		double param_ang_p_ff_;
		double param_ang_y_ff_;
		pidController controller_rates_x_;
		pidController controller_rates_y_;
		pidController controller_rates_z_;

		controlCommand_t control_angle_;
		controlCommand_t control_rates_;
		controlCommand_t control_forces_;

	public:
		void mixerCallback(const ros::TimerEvent& event) {
			msg_rc_out_.header.stamp = event.current_real;
			double dt = (event.current_real - event.last_real).toSec();

			if(!( goal_att_.type_mask & goal_att_.IGNORE_THRUST)) {
				control_angle_.T = goal_att_.thrust;
				control_rates_.T = goal_att_.thrust;
				control_forces_.T = goal_att_.thrust;
			} else {
				control_angle_.T = 0.0;
				control_rates_.T = 0.0;
				control_forces_.T = 0.0;
			}

			if(!( goal_att_.type_mask & goal_att_.IGNORE_ATTITUDE)) {
				tf2::Quaternion q_c( goal_att_.orientation.x,
									 goal_att_.orientation.y,
									 goal_att_.orientation.z,
									 goal_att_.orientation.w );

				tf2::Matrix3x3(q_c).getRPY(control_angle_.r, control_angle_.p, control_angle_.y);
			} else {
				control_angle_.r = 0.0;
				control_angle_.p = 0.0;
				control_angle_.y = 0.0;
			}

			double current_r = 0.0;
			double current_p = 0.0;
			double current_y = 0.0;

			tf2::Quaternion m_q_c( model_current_pose_.orientation.x,
								   model_current_pose_.orientation.y,
								   model_current_pose_.orientation.z,
								   model_current_pose_.orientation.w );

			tf2::Matrix3x3(m_q_c).getRPY(current_r, current_p, current_y);

			if(!( goal_att_.type_mask & goal_att_.IGNORE_ROLL_RATE )) {
				control_rates_.r = goal_att_.body_rate.x;
			} else {
				control_rates_.r = param_ang_r_ff_ * (control_angle_.r - current_r);
			}

			if(!( goal_att_.type_mask & goal_att_.IGNORE_PITCH_RATE )) {
				control_rates_.p = goal_att_.body_rate.y;
			} else {
				control_rates_.p = param_ang_p_ff_ * (control_angle_.p - current_p);
			}

			if(!( goal_att_.type_mask & goal_att_.IGNORE_YAW_RATE )) {
				control_rates_.y = goal_att_.body_rate.z;
			} else {
				control_rates_.y = param_ang_y_ff_ * (control_angle_.y - current_y);
			}

			control_forces_.r = controller_rates_x_.step( dt, control_rates_.r, model_current_twist_.angular.x );
			control_forces_.p = controller_rates_y_.step( dt, control_rates_.p, model_current_twist_.angular.y );
			control_forces_.y = controller_rates_z_.step( dt, control_rates_.y, model_current_twist_.angular.z );

			int32_t max_output = 1000;
			int32_t scale_factor = 1000;
			int32_t prescaled_outputs[NUM_MOTORS];
			int32_t pwm_output_requested[NUM_MOTORS];

			for (uint8_t i=0; i<NUM_MOTORS; i++) {

				double thrust_calc = ( control_forces_.T * mixer_hexacopter_x[i].throttle ) +
									 ( control_forces_.r * mixer_hexacopter_x[i].roll ) +
									 ( control_forces_.p * mixer_hexacopter_x[i].pitch ) +
									 ( control_forces_.y * mixer_hexacopter_x[i].yaw );


				prescaled_outputs[i] = (int)(thrust_calc * 1000);

				if( prescaled_outputs[i] > max_output )
					max_output = prescaled_outputs[i];

				//If the thrust is 0, zero motor outputs, as we don't want any thrust at all for safety
				if( control_rates_.T <= 0 )
					prescaled_outputs[i] = 0;
			}

			// saturate outputs to maintain controllability even during aggressive maneuvers
			if (max_output > 1000)
				scale_factor = 1000 * 1000 / max_output;

			for (uint8_t i=0; i<NUM_MOTORS; i++) {
					 int32_t channel_out = (prescaled_outputs[i] * scale_factor / 1000); // divide by scale factor
					 msg_rc_out_.channels[i] = 1000 + ( channel_out < 0 ? 0 : ( channel_out > 1000 ? 1000 : channel_out ) );
			}

			pub_rc_out_.publish(msg_rc_out_);
		}

		void odomCallback(const nav_msgs::Odometry::ConstPtr& msg_in) {
			model_current_pose_ = msg_in->pose.pose;
			model_current_twist_ = msg_in->twist.twist;
		}

		void attitudeTargetCallback( const mavros_msgs::AttitudeTarget::ConstPtr& msg_in ) {
			goal_att_ = *msg_in;
		}

		ControllerAcro() :
			nh_( "~" ),
			frame_id_("world"),
			model_name_("mantis_uav"),
			topic_attitude_target_("/mantis_uav/command/attitude_target"),
			topic_odom_("/mantis_uav/odom"),
			topic_rc_out_("/mantis_uav/command/motor_pwm"),
			pwm_update_rate_(100.0) {

			sub_attitude_target_ = nh_.subscribe<mavros_msgs::AttitudeTarget>(topic_attitude_target_, 1, &ControllerAcro::attitudeTargetCallback, this);
			sub_odom_ = nh_.subscribe<nav_msgs::Odometry>(topic_odom_, 1, &ControllerAcro::odomCallback, this);
			pub_rc_out_ = nh_.advertise<mavros_msgs::RCOut>( topic_rc_out_, 10 );

			tmr_rc_out_ = nh_.createTimer(ros::Duration(1 / pwm_update_rate_), &ControllerAcro::mixerCallback, this);

			param_ang_r_ff_ = 6.0;
			double param_pid_rate_x_kp = 0.15;
			double param_pid_rate_x_ki = 0.05;
			double param_pid_rate_x_kd = 0.003;
			double param_pid_rate_x_tau = 0.002;
			double param_pid_rate_x_max = 3.84;	//~220 deg/s

			param_ang_p_ff_ = 6.0;
			double param_pid_rate_y_kp = 0.15;
			double param_pid_rate_y_ki = 0.05;
			double param_pid_rate_y_kd = 0.003;
			double param_pid_rate_y_tau = 0.002;
			double param_pid_rate_y_max = 3.84;	//~220 deg/s

			param_ang_y_ff_ = 2.8;
			double param_pid_rate_z_kp = 0.2;
			double param_pid_rate_z_ki = 0.1;
			double param_pid_rate_z_kd = 0.0;
			double param_pid_rate_z_tau = 0.002;
			double param_pid_rate_z_max = 0.5; //~60 deg/s

			controller_rates_x_.setGains( param_pid_rate_x_kp, param_pid_rate_x_ki, param_pid_rate_x_kd, param_pid_rate_x_tau);
			controller_rates_x_.setOutputMinMax( -param_pid_rate_x_max, param_pid_rate_x_max );
			controller_rates_y_.setGains( param_pid_rate_y_kp, param_pid_rate_y_ki, param_pid_rate_y_kd, param_pid_rate_y_tau);
			controller_rates_y_.setOutputMinMax( -param_pid_rate_y_max, param_pid_rate_y_max );
			controller_rates_z_.setGains( param_pid_rate_z_kp, param_pid_rate_z_ki, param_pid_rate_z_kd, param_pid_rate_z_tau);
			controller_rates_z_.setOutputMinMax( -param_pid_rate_z_max, param_pid_rate_z_max );

			msg_rc_out_.header.frame_id = frame_id_;
			msg_rc_out_.channels.resize(NUM_MOTORS);

			ROS_INFO("Listening for outputting motor commands...");
		}

		~ControllerAcro() {
			//This message won't actually send here, as the node will have already shut down
			ROS_INFO("Shutting down...");
		}
};

int main(int argc, char** argv) {
	ros::init(argc, argv, "controller_acro");
	ControllerAcro ca;

	ros::spin();

	return 0;
}