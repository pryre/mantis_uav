/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <eigen3/Eigen/Dense>

inline void calc_Mm(Eigen::MatrixXd& Mm) {
	Mm(0,2) = 2.375861249703017E-2;
	Mm(1,2) = 2.375861249703017E-2;
	Mm(2,2) = 2.375861249703017E-2;
	Mm(3,2) = 2.375861249703017E-2;
	Mm(0,3) = -1.221809164235162E-1;
	Mm(1,3) = 1.221809164235162E-1;
	Mm(2,3) = 1.221809164235162E-1;
	Mm(3,3) = -1.221809164235162E-1;
	Mm(0,4) = -1.221809164235162E-1;
	Mm(1,4) = 1.221809164235162E-1;
	Mm(2,4) = -1.221809164235162E-1;
	Mm(3,4) = 1.221809164235162E-1;
	Mm(0,5) = 5.0;
	Mm(1,5) = 5.0;
	Mm(2,5) = -5.0;
	Mm(3,5) = -5.0;
	Mm(4,6) = 1.0;
	Mm(5,7) = 1.0;
}
