// Copyright 2026, OpenAI
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Eigen/Geometry>

namespace wivrn
{
// OpenXR angular velocity is expressed in the reference space. Both the
// polynomial's quaternion and its derivative use scalar-first (w, x, y, z).
inline Eigen::Vector4f quaternion_derivative(const Eigen::Quaternionf & q, const Eigen::Vector3f & velocity)
{
	const Eigen::Quaternionf omega{0, velocity.x(), velocity.y(), velocity.z()};
	const Eigen::Quaternionf dq = omega * q;
	return 0.5f * Eigen::Vector4f{dq.w(), dq.x(), dq.y(), dq.z()};
}
}
