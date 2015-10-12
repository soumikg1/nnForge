/*
 *  Copyright 2011-2015 Maxim Milakov
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#pragma once

#include "layer_configuration.h"
#include "layer_configuration_specific.h"
#include "layer_data.h"
#include "layer_data_custom.h"
#include "rnd.h"
#include "layer_data_configuration.h"
#include "nn_types.h"
#include "tiling_factor.h"

#include <ostream>
#include <istream>
#include <set>

namespace nnforge
{
	typedef std::vector<unsigned int> data_config;
	typedef std::vector<unsigned int> data_custom_config;

	class layer
	{
	public:
		typedef nnforge_shared_ptr<layer> ptr;
		typedef nnforge_shared_ptr<const layer> const_ptr;

		virtual ~layer();

		virtual layer::ptr clone() const = 0;

		virtual layer_configuration get_layer_configuration(const std::vector<layer_configuration>& input_configuration_list) const;

		virtual layer_configuration_specific get_output_layer_configuration_specific(const std::vector<layer_configuration_specific>& input_configuration_specific_list) const;

		virtual layer_configuration_specific get_input_layer_configuration_specific(
			const layer_configuration_specific& output_configuration_specific,
			unsigned int input_layer_id) const;

		// Returns minimal input rectangle which this layer quasi-transforms into output one covering the one supplied as an argument to the function
		// "Quasi" means that we don't take into account "soft" effects from nearby neurons, for example when doing local contrast subtracting blurred version
		virtual std::vector<std::pair<unsigned int, unsigned int> > get_input_rectangle_borders(
			const std::vector<std::pair<unsigned int, unsigned int> >& output_rectangle_borders,
			unsigned int input_layer_id) const;

		virtual layer_data_configuration_list get_layer_data_configuration_list() const;

		virtual float get_forward_flops(const std::vector<layer_configuration_specific>& input_configuration_specific_list) const = 0;

		virtual float get_backward_flops(
			const std::vector<layer_configuration_specific>& input_configuration_specific_list,
			unsigned int input_layer_id) const = 0;

		virtual float get_weights_update_flops(const std::vector<layer_configuration_specific>& input_configuration_specific_list) const;

		virtual std::string get_type_name() const = 0;

		virtual void read_proto(const void * layer_proto);

		virtual void write_proto(void * layer_proto) const;

		// All values are set to 0.0F
		layer_data::ptr create_layer_data() const;

		// All values are set to -1
		layer_data_custom::ptr create_layer_data_custom() const;

		// The method throws exception in case the data is not suitable for the layer
		void check_layer_data_consistency(const layer_data& data) const;

		// The method throws exception in case the data is not suitable for the layer
		void check_layer_data_custom_consistency(const layer_data_custom& data_custom) const;

		// Override this member function to randomize data
		virtual void randomize_data(
			layer_data::ptr data,
			layer_data_custom::ptr data_custom,
			random_generator& generator) const;

		// Override this member function to randomize data
		virtual void randomize_orthogonal_data(
			layer_data::ptr data,
			layer_data_custom::ptr data_custom,
			random_generator& generator) const;

		virtual std::set<unsigned int> get_weight_decay_part_id_set() const;

		bool is_empty_data() const;

		bool is_empty_data_custom() const;

		virtual std::vector<tiling_factor> get_tiling_factor_list() const;

		tiling_factor get_tiling_factor() const;

		virtual std::string get_string_for_average_data(
			const layer_configuration_specific& config,
			const std::vector<float>& data) const;

	public:
		std::string instance_name;
		std::vector<std::string> input_layer_instance_names;

	protected:
		layer();

		virtual data_config get_data_config() const;

		virtual data_custom_config get_data_custom_config() const;
	};
}
