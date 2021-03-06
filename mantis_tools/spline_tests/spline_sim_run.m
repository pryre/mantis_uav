% This Source Code Form is subject to the terms of the Mozilla Public
% License, v. 2.0. If a copy of the MPL was not distributed with this
% file, You can obtain one at https://mozilla.org/MPL/2.0/.

% This Source Code Form is subject to the terms of the Mozilla Public
% License, v. 2.0. If a copy of the MPL was not distributed with this
% file, You can obtain one at https://mozilla.org/MPL/2.0/.

function [dx] = spline_sim_run(x,v,mass)
%PCT_RUN Summary of this function goes here
%   Detailed explanation goes here
    
    % Variables
    dx = zeros(size(x));
    g = 9.80665;
    
    % Calculate force input (torque) using computed torque control
    u = (mass.Iyy)*v; %(mass.Iyy+0.02)*v;
    
    % Dynamics
    dx(1) = x(2);
    dx(2) = g*tan(x(3));
    dx(3) = x(4);
    dx(4) = u/mass.Iyy;
end

