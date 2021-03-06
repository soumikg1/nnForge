/*
 *  Copyright 2011-2016 Maxim Milakov
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

#include "untile_layer.h"

#include "neural_network_exception.h"
#include "proto/nnforge.pb.h"

#include <algorithm>
#include <boost/lambda/lambda.hpp>
#include <boost/format.hpp>
#include <sstream>

namespace nnforge
{
	const std::string untile_layer::layer_type_name = "UnTile";

	untile_layer::untile_layer(const std::vector<std::vector<unsigned int> >& upsampling_sizes_list)
		: upsampling_sizes_list(upsampling_sizes_list)
	{
		check();
	}

	void untile_layer::check()
	{
		if (upsampling_sizes_list.size() == 0)
			throw neural_network_exception("level list for untile layer may not be empty");

		unsigned int dimension_count = static_cast<unsigned int>(upsampling_sizes_list.front().size());
		if (dimension_count == 0)
			throw neural_network_exception("upsampling size for untile layer may not be zero");
		for(unsigned int i = 1; i < static_cast<unsigned int>(upsampling_sizes_list.size()); i++)
		{
			unsigned int new_dimension_count = static_cast<unsigned int>(upsampling_sizes_list[i].size());
			if (dimension_count != new_dimension_count)
				throw neural_network_exception("window dimensions for max subsampling layer should be equal for all levels");
		}
	}

	std::string untile_layer::get_type_name() const
	{
		return layer_type_name;
	}

	layer::ptr untile_layer::clone() const
	{
		return layer::ptr(new untile_layer(*this));
	}

	layer_configuration_specific untile_layer::get_output_layer_configuration_specific(const std::vector<layer_configuration_specific>& input_configuration_specific_list) const
	{
		if (input_configuration_specific_list[0].get_dimension_count() != upsampling_sizes_list.front().size())
			throw neural_network_exception((boost::format("Dimension count in layer (%1%) and input configuration (%2%) don't match") % upsampling_sizes_list.front().size() % input_configuration_specific_list[0].get_dimension_count()).str());

		layer_configuration_specific res(input_configuration_specific_list[0]);

		for(unsigned int i = 0; i < upsampling_sizes_list.size(); ++i)
		{
			const std::vector<unsigned int>& upsampling_sizes = upsampling_sizes_list[i];
			for(unsigned int j = 0; j < upsampling_sizes.size(); ++j)
				res.dimension_sizes[j] *= upsampling_sizes[j];
		}

		return res;
	}

	bool untile_layer::get_input_layer_configuration_specific(
		layer_configuration_specific& input_configuration_specific,
		const layer_configuration_specific& output_configuration_specific,
		unsigned int input_layer_id) const
	{
		if (output_configuration_specific.get_dimension_count() != upsampling_sizes_list.front().size())
			throw neural_network_exception((boost::format("Dimension count in layer (%1%) and output configuration (%2%) don't match") % upsampling_sizes_list.front().size() % output_configuration_specific.get_dimension_count()).str());

		input_configuration_specific = layer_configuration_specific(output_configuration_specific);

		for(unsigned int i = 0; i < upsampling_sizes_list.size(); ++i)
		{
			const std::vector<unsigned int>& upsampling_sizes = upsampling_sizes_list[i];
			for(unsigned int j = 0; j < upsampling_sizes.size(); ++j)
			{
				if (input_configuration_specific.dimension_sizes[j] % upsampling_sizes[j] == 0)
					input_configuration_specific.dimension_sizes[j] /= upsampling_sizes[j];
				else
					throw neural_network_exception("upsampling sizes of untile layer cannot evenly divide output sizes");
			}
		}

		return true;
	}

	void untile_layer::write_proto(void * layer_proto) const
	{
		protobuf::Layer * layer_proto_typed = reinterpret_cast<protobuf::Layer *>(layer_proto);
		protobuf::UnTileParam * param = layer_proto_typed->mutable_untile_param();

		for(int i = 0; i < upsampling_sizes_list.size(); ++i)
		{
			const std::vector<unsigned int>& upsampling_sizes = upsampling_sizes_list[i];
			protobuf::UnTileParam_UnTileLevelParam * level_param = param->add_level_param();
			for(int j = 0; j < upsampling_sizes.size(); ++j)
			{
				protobuf::UnTileParam_UnTileUpsamplingDimensionParam * dim_param = level_param->add_dimension_param();
				dim_param->set_upsampling_size(upsampling_sizes[j]);
			}
		}
	}

	void untile_layer::read_proto(const void * layer_proto)
	{
		const protobuf::Layer * layer_proto_typed = reinterpret_cast<const protobuf::Layer *>(layer_proto);
		if (!layer_proto_typed->has_untile_param())
			throw neural_network_exception((boost::format("No untile_param specified for layer %1% of type %2%") % instance_name % layer_proto_typed->type()).str());

		upsampling_sizes_list.resize(layer_proto_typed->untile_param().level_param_size());
		for(int i = 0; i < layer_proto_typed->untile_param().level_param_size(); ++i)
		{
			const protobuf::UnTileParam_UnTileLevelParam& level_param = layer_proto_typed->untile_param().level_param(i);
			std::vector<unsigned int>& upsampling_sizes = upsampling_sizes_list[i];
			upsampling_sizes.resize(level_param.dimension_param_size());
			for(int j = 0; j < level_param.dimension_param_size(); ++j)
				upsampling_sizes[j] = level_param.dimension_param(j).upsampling_size();
		}

		check();
	}

	float untile_layer::get_flops_per_entry(
		const std::vector<layer_configuration_specific>& input_configuration_specific_list,
		const layer_action& action) const
	{
		switch (action.get_action_type())
		{
		case layer_action::forward:
			return 0.0F;
		case layer_action::backward_data:
			return 0.0F;
		default:
			return 0.0F;
		}
	}

	tiling_factor untile_layer::get_tiling_factor() const
	{
		std::vector<tiling_factor> tiling_factor_list = get_tiling_factor_list();

		tiling_factor res = 1;
		std::for_each(tiling_factor_list.begin(), tiling_factor_list.end(), res *= boost::lambda::_1);

		return res;
	}

	std::vector<tiling_factor> untile_layer::get_tiling_factor_list() const
	{
		std::vector<tiling_factor> res(upsampling_sizes_list.front().size(), 1);

		for(int i = 0; i < upsampling_sizes_list.size(); ++i)
		{
			const std::vector<unsigned int>& upsampling_sizes = upsampling_sizes_list[i];
			for(int j = 0; j < upsampling_sizes.size(); ++j)
				res[j] = res[j] * tiling_factor(upsampling_sizes[j], false);
		}

		return res;
	}

	std::vector<std::string> untile_layer::get_parameter_strings() const
	{
		std::vector<std::string> res;

		std::stringstream ss;

		ss << "upsampling ";
		for(int i = 0; i < upsampling_sizes_list.size(); ++i)
		{
			if (i != 0)
				ss << ", ";
			const std::vector<unsigned int>& upsampling_sizes = upsampling_sizes_list[i];
			for(int j = 0; j < upsampling_sizes.size(); ++j)
			{
				if (j != 0)
					ss << "x";
				ss << upsampling_sizes[j];
			}
		}

		res.push_back(ss.str());

		return res;
	}
}
