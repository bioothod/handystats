/*
* Copyright (c) YANDEX LLC. All rights reserved.
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 3.0 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library.
*/

#include <iostream>
#include <string>
#include <thread>
#include <future>
#include <vector>
#include <chrono>

#include <boost/program_options.hpp>
#include <boost/random/linear_congruential.hpp>
#include <boost/random/uniform_int.hpp>
#include <boost/random/uniform_real.hpp>
#include <boost/random/variate_generator.hpp>

#include <handystats/math_utils.hpp>
#include <handystats/core.hpp>
#include <handystats/metrics_dump.hpp>
#include <handystats/measuring_points.hpp>

#include "command_executor.hpp"

uint64_t threads = 1;
uint64_t events = 1;

int main(int argc, char** argv) {
	namespace po = boost::program_options;
	po::options_description desc("Options");
	desc.add_options()
		("help", "Print help messages")
		("handystats-config", po::value<std::string>(),
			"Handystats configuration (in JSON format)"
		)
		("threads", po::value<uint64_t>(&threads)->default_value(threads),
			"Number of worker threads"
		)
		("events", po::value<uint64_t>(&events)->default_value(events),
			"Number of events of each thread"
		)
	;

	po::variables_map vm;
	try {
		po::store(po::parse_command_line(argc, argv, desc), vm);
		if (vm.count("help")) {
			std::cout << desc << std::endl;
			return 0;
		}
		po::notify(vm);
	}
	catch(po::error& e) {
		std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
		std::cerr << desc << std::endl;
		return 1;
	}

	if (vm["threads"].as<uint64_t>() == 0) {
		std::cerr << "ERROR: number of threads must be greater than 0" << std::endl;
		return 1;
	}

	if (vm["events"].as<uint64_t>() == 0) {
		std::cerr << "ERROR: number of events must be greater than 0" << std::endl;
		return 1;
	}

	if (vm.count("handystats-config")) {
		HANDY_CONFIG_JSON(vm["handystats-config"].as<std::string>().c_str());
	}
	else {
		HANDY_CONFIG_JSON("{\"enable\": true, \"events\": {\"tags\": [\"count\"]}}");
	}

	HANDY_INIT();

	auto start = handystats::chrono::tsc_clock::now();

	std::vector<std::thread> workers(threads);
	int id = 0;
	for (auto& worker : workers) {
		++id;
		worker = std::thread(
			[id] () {
				double value = id;
				for (uint64_t i = 1; i <= events; ++i) {
					value *= (i + id);
					HANDY_GAUGE_SET("events", value);
				}
			}
		);
	}

	for (auto& worker : workers) {
		worker.join();
	}

	auto end = handystats::chrono::tsc_clock::now();

	std::this_thread::sleep_for(std::chrono::seconds(15));

	std::cout << "Workers time: " << handystats::chrono::duration::convert_to(handystats::chrono::time_unit::SEC, end - start).count() << "s" << std::endl;
	std::cout << "Total events: " << threads * events << std::endl;
	auto metrics_dump = HANDY_METRICS_DUMP();
	const auto& processed_events = boost::get<handystats::metrics::gauge>(metrics_dump->at("events"));
	std::cout << "Processed events: " << processed_events.values().get<handystats::statistics::tag::count>() << std::endl;
	const auto& message_queue_size = boost::get<handystats::metrics::gauge>(metrics_dump->at("handystats.message_queue.size"));
	std::cout << "Queue size: " << message_queue_size.values().get<handystats::statistics::tag::value>() << std::endl;

	HANDY_FINALIZE();
}
