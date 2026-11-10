// Copyright 2026, OpenAI
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include "driver/polynomial_interpolator.h"
#include "driver/quaternion_derivative.h"

using Interpolator = polynomial_interpolator<4, true>;
constexpr XrTime start = 10'000'000'000;

static void check(bool condition)
{
	if (!condition)
		std::abort();
}

static Interpolator::sample sample(int i, const Eigen::Vector3f & axis)
{
	Eigen::Quaternionf q{Eigen::AngleAxisf(i * 0.01f, axis)};
	Interpolator::sample s{start + i * 10'000'000, start + i * 10'000'000};
	s.y.emplace(q.w(), q.x(), q.y(), q.z());
	s.dy = wivrn::quaternion_derivative(q, axis);
	return s;
}

int main()
{
	for (const Eigen::Vector3f axis: {Eigen::Vector3f::UnitX().eval(), Eigen::Vector3f::UnitY().eval(),
	                                Eigen::Vector3f::UnitZ().eval(), Eigen::Vector3f{1, 2, 3}.normalized().eval()})
	{
		// Startup with one sample exercises velocity rather than letting a long
		// position history hide a wrong derivative. Repeat with a short history.
		for (int count: {1, 2, 10})
		{
			Interpolator p;
			for (int i = 0; i < count; ++i)
				p.add_sample(sample(i, axis));
			auto result = p.get_at(start + (count - 1) * 10'000'000 + 20'000'000);
			check(result.y && result.dy);
			auto v = result.y->normalized().eval();
			Eigen::Quaternionf q{v[0], v[1], v[2], v[3]};
			Eigen::Quaternionf expected{Eigen::AngleAxisf((count - 1) * 0.01f + 0.02f, axis)};
			check(q.angularDistance(expected) < 0.0001f);
			Eigen::Quaternionf dq{(*result.dy)[0], (*result.dy)[1], (*result.dy)[2], (*result.dy)[3]};
			Eigen::Vector3f velocity = 2 * (dq * q.conjugate()).vec();
			check((velocity - axis).norm() < 0.01f);
		}
	}
	for (bool replace: {false, true})
	{
		Interpolator consistent, alternating;
		for (int i = 0; i < 10; ++i)
		{
			auto s = sample(i, Eigen::Vector3f::UnitY());
			consistent.add_sample(s);
			if (replace)
				alternating.add_sample(s);
			if (i % 2)
			{
				*s.y *= -1;
				*s.dy *= -1;
			}
			// Replacement and insertion must align the derivative with q.
			++s.production_timestamp;
			alternating.add_sample(s);
			for (XrDuration dt: {XrDuration{0}, XrDuration{20'000'000}})
			{
				auto a = consistent.get_at(s.timestamp + dt), b = alternating.get_at(s.timestamp + dt);
				check(a.y && b.y && a.dy && b.dy);
				check((*a.y - *b.y).norm() < 1e-5f);
				check((*a.dy - *b.dy).norm() < 1e-4f);
			}
		}
	}
	std::cout << "Quaternion interpolation tests passed\n";
}
