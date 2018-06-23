#include <eigen3/Eigen/Dense>

#include <geometry_msgs/Pose.h>
#include <geometry_msgs/Twist.h>
#include <geometry_msgs/Point.h>
#include <geometry_msgs/Vector3.h>
#include <geometry_msgs/Quaternion.h>

//Math Helpers
inline Eigen::Matrix3d vee_up(const Eigen::Vector3d& w) {
	Eigen::Matrix3d W;

	W <<  0.0, -w(2),  w(1),
		 w(2),   0.0, -w(0),
		-w(1),  w(0),   0.0;

	return W;
}

inline Eigen::Vector3d vee_down(const Eigen::Matrix3d& W) {
	return Eigen::Vector3d(W(2,1), W(0,2), W(1,0));
}

double double_clamp(const double v, const double min, const double max) {
	return (v < min) ? min : (v > max) ? max : v;
}

void matrix_clamp(Eigen::MatrixXd &m, const double min, const double max) {
	for(int i=0; i<m.rows(); i++) {
		for(int j=0; j<m.cols(); j++) {
			m(i,j) = double_clamp(m(i,j), min, max);
		}
	}
}

//Matrix Calculation Helpers
Eigen::Matrix3d extract_yaw_component(const Eigen::Matrix3d &r) {
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

//Conversion Helpers
inline Eigen::Vector3d vector_from_msg(const geometry_msgs::Vector3 &v) {
		return Eigen::Vector3d(v.x, v.y, v.z);
}

inline Eigen::Vector3d point_from_msg(const geometry_msgs::Point &p) {
		return Eigen::Vector3d(p.x, p.y, p.z);
}

inline Eigen::Quaterniond quaternion_from_msg(const geometry_msgs::Quaternion &q) {
		return Eigen::Quaterniond(q.w, q.x, q.y, q.z).normalized();
}

inline Eigen::Affine3d affine_from_msg(const geometry_msgs::Pose &pose) {
		Eigen::Affine3d a;

		a.translation() << point_from_msg(pose.position);
		a.linear() << quaternion_from_msg(pose.orientation).toRotationMatrix();

		return a;
}

inline geometry_msgs::Vector3 vector_from_eig(const Eigen::Vector3d &v) {
	geometry_msgs::Vector3 vec;

	vec.x = v.x();
	vec.y = v.y();
	vec.z = v.z();

	return vec;
}

inline geometry_msgs::Point point_from_eig(const Eigen::Vector3d &p) {
	geometry_msgs::Point point;

	point.x = p.x();
	point.y = p.y();
	point.z = p.z();

	return point;
}

inline geometry_msgs::Quaternion quaternion_from_eig(const Eigen::Quaterniond &q) {
	geometry_msgs::Quaternion quat;
	Eigen::Quaterniond qn = q.normalized();

	quat.w = qn.w();
	quat.x = qn.x();
	quat.y = qn.y();
	quat.z = qn.z();

	return quat;
}

inline geometry_msgs::Pose pose_from_eig(const Eigen::Affine3d &g) {
	geometry_msgs::Pose pose;

	pose.position = point_from_eig(g.translation());
	pose.orientation = quaternion_from_eig(Eigen::Quaterniond(g.linear()));

	return pose;
}