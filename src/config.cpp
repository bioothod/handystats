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

#include <mutex>
#include <fstream>
#include <string>
#include <iostream>
#include <sstream>

#include <fnmatch.h>

#include <handystats/core.hpp>
#include <handystats/core.h>

#include <rapidjson/document.h>

#include "core_impl.hpp"

#include "config_impl.hpp"

namespace {

rapidjson::Document& get_source() {
	static rapidjson::Document source;
	return source;
}

} // namespace

namespace handystats { namespace config {

/*
* pattern ::= [<preabmle>] [ { <list> } ] [<postscript>]
*
* preamble ::= <word>
*
* word ::= sequence without <special_symbol> (may be empty)
* special_symbol ::= { | } | ,
*
* list ::= <pattern> [, <list>]
*
* postscript ::= <pattern>
*/
// returns pattern expansion as vector of strings
// if expansion fails empty vector is returned
// else pos points to end of pattern
//
// may throw std::logic_error
static
std::vector<std::string> expand_pattern(const char* pattern, size_t& pos, bool nested) {
	static const char SYM_LIST_DELIM = ',';
	static const char SYM_LIST_START = '{';
	static const char SYM_LIST_END = '}';
	size_t preamble_end = pos;
	while (true) {
		if (pattern[preamble_end] == 0) {
			break;
		}
		if (pattern[preamble_end] == SYM_LIST_START) {
			break;
		}
		if (nested && (pattern[preamble_end] == SYM_LIST_END || pattern[preamble_end] == SYM_LIST_DELIM)) {
			break;
		}
		++preamble_end;
	}
	std::string preamble(pattern + pos, preamble_end - pos);
	if (pattern[preamble_end] != SYM_LIST_START) {
		// pattern is preamble
		pos = preamble_end;
		return {preamble};
	}
	std::vector<std::string> list_expansion;
	size_t list_end = preamble_end + 1;
	while (true) {
		auto sublist_expansion = expand_pattern(pattern, list_end, true);
		list_expansion.insert(list_expansion.end(), sublist_expansion.begin(), sublist_expansion.end());
		if (pattern[list_end] == SYM_LIST_DELIM) {
			list_end++;
			continue;
		}
		else if (pattern[list_end] == SYM_LIST_END) {
			break;
		}
		else {
			std::ostringstream err;
			err << "Invalid symbol at pos " << list_end << " pattern '" << pattern << "'";
			throw std::logic_error(err.str());
		}
	}
	size_t postscript_start = list_end + 1;
	auto postscript_expansion = expand_pattern(pattern, postscript_start, nested);
	std::vector<std::string> expansion;
	expansion.reserve(list_expansion.size() * postscript_expansion.size());
	for (auto list_iter = list_expansion.begin(); list_iter != list_expansion.end(); ++list_iter) {
		for (auto postscript_iter = postscript_expansion.begin(); postscript_iter != postscript_expansion.end(); ++postscript_iter) {
			expansion.push_back(preamble + *list_iter + *postscript_iter);
		}
	}
	pos = postscript_start;
	return expansion;
}

static
std::vector<std::string> expand_pattern(const std::string& pattern) {
	size_t pos = 0;
	auto expansion = expand_pattern(pattern.c_str(), pos, false);
	if (pos == pattern.size()) {
		return expansion;
	}
	else {
		std::ostringstream err;
		err << "Parsing stopped at pos " << pos << " pattern '" << pattern << "'";
		throw std::logic_error(err.str());
	}
}

rapidjson::Value* select_pattern(const std::string& name) {
	for (auto pattern_group_iter = pattern_opts.begin(); pattern_group_iter != pattern_opts.end(); ++pattern_group_iter) {
		auto& pattern_group = pattern_group_iter->first;
		auto* pattern_cfg = pattern_group_iter->second;
		for (auto pattern_iter = pattern_group.begin(); pattern_iter != pattern_group.end(); ++pattern_iter) {
			if (fnmatch(pattern_iter->c_str(), name.c_str(), 0) == 0) {
				return pattern_cfg;
			}
		}
	}

	return nullptr;
}


statistics statistics_opts;

namespace metrics {
	gauge gauge_opts;
	counter counter_opts;
	timer timer_opts;
}

metrics_dump metrics_dump_opts;
core core_opts;

std::vector<
	std::pair<
		std::vector<std::string>,
		rapidjson::Value*
	>
>
pattern_opts;

static void reset() {
	statistics_opts = statistics();

	metrics::gauge_opts = metrics::gauge();
	metrics::counter_opts = metrics::counter();
	metrics::timer_opts = metrics::timer();

	metrics_dump_opts = metrics_dump();
	core_opts = core();

	pattern_opts.clear();
	rapidjson::Document doc;
	get_source().Swap(doc);
}

__attribute__((constructor(300)))
static void init_opts() {
	reset();
}

void initialize() {
	reset();
}

void finalize() {
	reset();
}

}} // namespace handystats::config


namespace handystats {

bool config_json(const char* config_data) {
	std::lock_guard<std::mutex> lock(handystats::operation_mutex);
	if (handystats::is_enabled()) {
		return true;
	}

	rapidjson::Document& cfg = get_source();

	cfg.Parse<0>(config_data);
	if (cfg.HasParseError()) {
		std::cerr << "Unable to parse configuration json: " << cfg.GetParseError() << std::endl;
		return false;
	}

	if (!cfg.IsObject()) {
		return false;
	}

	/*
	 * old configuration format
	 *
	 * {
	 *   "statistics": {
	 *     ...
	 *   },
	 *
	 *   "metrics": {
	 *     "gauge": {
	 *       ...
	 *     },
	 *     "counter": {
	 *       ...
	 *     },
	 *     "timer": {
	 *       ...
	 *     }
	 *   },
	 *
	 *   "metrics-dump": {
	 *     "interval": ...
	 *   },
	 *
	 *   "core": {
	 *     "enable": ...
	 *   }
	 * }
	 */

	if (cfg.HasMember("statistics")) {
		const rapidjson::Value& statistics_config = cfg["statistics"];

		configure(config::statistics_opts, statistics_config);

		configure(config::metrics::gauge_opts.values, statistics_config);

		configure(config::metrics::counter_opts.values, statistics_config);

		configure(config::metrics::timer_opts.values, statistics_config);
	}

	if (cfg.HasMember("metrics")) {
		const rapidjson::Value& metrics_config = cfg["metrics"];

		if (metrics_config.HasMember("gauge")) {
			configure(config::metrics::gauge_opts, metrics_config["gauge"]);
		}
		if (metrics_config.HasMember("counter")) {
			configure(config::metrics::counter_opts, metrics_config["counter"]);
		}
		if (metrics_config.HasMember("timer")) {
			configure(config::metrics::timer_opts, metrics_config["timer"]);
		}
	}

	if (cfg.HasMember("metrics-dump")) {
		const rapidjson::Value& metrics_dump_config = cfg["metrics-dump"];
		config::metrics_dump_opts.configure(metrics_dump_config);
	}


	if (cfg.HasMember("core")) {
		const rapidjson::Value& core_config = cfg["core"];
		config::core_opts.configure(core_config);
	}

	/*
	 * new configuration format
	 *
	 * {
	 *   "defaults": {
	 *     ...
	 *   },
	 *
	 *   "gauge": {
	 *     ...
	 *   },
	 *   "counter": {
	 *     ...
	 *   },
	 *   "timer": {
	 *     ...
	 *   },
	 *
	 *   "dump-interval": ...,
	 *
	 *   "enable": ...
	 * }
	 */

	if (cfg.HasMember("defaults")) {
		const rapidjson::Value& statistics_config = cfg["defaults"];

		configure(config::statistics_opts, statistics_config);

		configure(config::metrics::gauge_opts.values, statistics_config);

		configure(config::metrics::counter_opts.values, statistics_config);

		configure(config::metrics::timer_opts.values, statistics_config);
	}

	if (cfg.HasMember("gauge")) {
		const rapidjson::Value& gauge_config = cfg["gauge"];
		configure(config::metrics::gauge_opts, gauge_config);
	}

	if (cfg.HasMember("counter")) {
		const rapidjson::Value& counter_config = cfg["counter"];
		configure(config::metrics::counter_opts, counter_config);
	}

	if (cfg.HasMember("timer")) {
		const rapidjson::Value& timer_config = cfg["timer"];
		configure(config::metrics::timer_opts, timer_config);
	}

	if (cfg.HasMember("dump-interval")) {
		const rapidjson::Value& dump_interval = cfg["dump-interval"];

		if (dump_interval.IsUint64()) {
			config::metrics_dump_opts.interval = chrono::duration(dump_interval.GetUint64(), chrono::time_unit::MSEC);
		}
	}


	if (cfg.HasMember("enable")) {
		const rapidjson::Value& core_enable = cfg["enable"];

		if (core_enable.IsBool()) {
			config::core_opts.enable = core_enable.GetBool();
		}
	}

	/*
	 * pattern configuration format
	 *
	 * {
	 *   "<pattern>": {
	 *     ...
	 *   }
	 * }
	 */

	for (auto config_member = cfg.MemberBegin(); config_member != cfg.MemberEnd(); ++config_member) {
		rapidjson::Value& member_name = config_member->name;
		rapidjson::Value& member_value = config_member->value;

		// skip reserved sections
		if (
				// old configuration format sections
				strcmp(member_name.GetString(), "statistics") == 0
				|| strcmp(member_name.GetString(), "metrics") == 0
				|| strcmp(member_name.GetString(), "metrics-dump") == 0
				|| strcmp(member_name.GetString(), "core") == 0
				// new configuration format sections
				|| strcmp(member_name.GetString(), "defaults") == 0
				|| strcmp(member_name.GetString(), "gauge") == 0
				|| strcmp(member_name.GetString(), "counter") == 0
				|| strcmp(member_name.GetString(), "timer") == 0
				|| strcmp(member_name.GetString(), "dump-interval") == 0
				|| strcmp(member_name.GetString(), "enable") == 0
		   )
		{
			continue;
		}

		// pattern
		try {
			std::vector<std::string> expansion = config::expand_pattern(member_name.GetString());

			config::pattern_opts.push_back(std::make_pair(expansion, &member_value));
		}
		catch (const std::logic_error& err) {
			return false;
		}
	}

	return true;
}

bool config_file(const char* filename) {
	std::ifstream input(filename, std::ios::in | std::ios::binary);
	if (!input) {
		std::cerr << "Unable to open configuration file " << filename << std::endl;
		return false;
	}

	std::string config_data;
	input.seekg(0, std::ios::end);
	config_data.resize(input.tellg());
	input.seekg(0, std::ios::beg);
	input.read(&config_data[0], config_data.size());
	input.close();

	return config_json(config_data.c_str());
}

} // namespace handystats


extern "C" {

int handystats_config_file(const char* file) {
	return handystats::config_file(file);
}

int handystats_config_json(const char* config_data) {
	return handystats::config_json(config_data);
}

} // extern "C"
