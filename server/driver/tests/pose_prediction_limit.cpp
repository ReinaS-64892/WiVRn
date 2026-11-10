// SPDX-License-Identifier: GPL-3.0-or-later
#include "driver/polynomial_interpolator.h"
#include "pose_prediction.h"
#include <cstdlib>
#include <iostream>
#include <source_location>

static void check(bool condition, std::source_location location = std::source_location::current())
{
	if (!condition)
	{
		std::cerr << "Prediction limit assertion failed at line " << location.line() << '\n';
		std::abort();
	}
}

int main()
{
	using nlohmann::json;
	for (auto value: {json(nullptr), json("20"), json(true), json(-2), json(101), json(1.5), json(UINT64_MAX)})
		check(wivrn::pose_prediction_limit(json{{"limit", value}}, "limit") == -1);
	check(wivrn::pose_prediction_limit(json::object(), "limit") == -1);
	for (int value: {-1, 0, 20, 100})
		check(wivrn::pose_prediction_limit(json{{"limit", value}}, "limit") == value);

	check(wivrn::pose_prediction_limit(json::parse(R"({"limit":20})"), "limit") == 20);

	constexpr XrTime produced = 10'000'000'000;
	polynomial_interpolator<3> positions;
	// Headset has already predicted 30 ms ahead. Limit relative to production,
	// not to the timestamp of that predicted sample (which would double predict).
	for (int i = -2; i <= 0; ++i)
	{
		const XrTime production = produced + i * 10'000'000;
		const XrTime timestamp = production + 30'000'000;
		polynomial_interpolator<3>::sample sample{production, timestamp};
		sample.y.emplace(float(timestamp - produced) * 1e-9f, 0, 0);
		sample.dy.emplace(1, 0, 0);
		positions.add_sample(sample);
	}
	for (XrDuration limit: {XrDuration{0}, XrDuration{20'000'000}})
	{
		auto limited = positions.get_at(produced + 70'000'000, limit);
		check(limited.y && limited.dy);
		check(limited.timestamp == produced + limit);
		check(std::abs(limited.y->x() - limit * 1e-9f) < 1e-5f);
		check(std::abs(limited.dy->x() - 1) < 1e-3f);
		// Historical queries must not be advanced to the limit.
		auto past = positions.get_at(produced - 5'000'000, limit);
		check(past.y && past.timestamp == produced - 5'000'000);
	}
	auto unlimited = positions.get_at(produced + 70'000'000);
	check(unlimited.y && unlimited.timestamp == produced + 70'000'000);
	check(std::abs(unlimited.y->x() - 0.07f) < 1e-4f);
	positions.reset();
	check(!positions.get_at(produced, 0).y);
	polynomial_interpolator<3>::sample lone{produced, produced + 30'000'000};
	lone.y.emplace(0.03f, 0, 0);
	positions.add_sample(lone);
	check(!positions.get_at(produced + 70'000'000, 0).y);
	check(positions.get_at(produced + 70'000'000).y.has_value());
	lone.dy.emplace(1, 0, 0);
	++lone.production_timestamp;
	positions.add_sample(lone);
	auto resolved = positions.get_at(produced + 70'000'000, 0);
	check(resolved.y && std::abs(resolved.y->x()) < 1e-5f);
	std::cout << "Pose prediction limits: passed\n";
}
