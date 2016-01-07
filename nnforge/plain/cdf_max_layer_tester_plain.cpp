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

#include "cdf_max_layer_tester_plain.h"

#include "../cdf_max_layer.h"
#include "../nn_types.h"

#include <array>

namespace nnforge
{
	namespace plain
	{
		cdf_max_layer_tester_plain::cdf_max_layer_tester_plain()
		{
		}

		cdf_max_layer_tester_plain::~cdf_max_layer_tester_plain()
		{
		}

		std::string cdf_max_layer_tester_plain::get_type_name() const
		{
			return cdf_max_layer::layer_type_name;
		}

		void cdf_max_layer_tester_plain::run_forward_propagation(
			plain_buffer::ptr output_buffer,
			const std::vector<plain_buffer::const_ptr>& input_buffers,
			plain_buffer::ptr temporary_working_fixed_buffer,
			plain_buffer::ptr temporary_working_per_entry_buffer,
			plain_running_configuration::const_ptr plain_config,
			layer::const_ptr layer_schema,
			layer_data::const_ptr data,
			layer_data_custom::const_ptr data_custom,
			const std::vector<layer_configuration_specific>& input_configuration_specific_list,
			const layer_configuration_specific& output_configuration_specific,
			unsigned int entry_count) const
		{
			const float * const in_it_global = *input_buffers[0];
			float * const out_it_global = *output_buffer;
			const unsigned int neuron_count = output_configuration_specific.get_neuron_count();
			nnforge_shared_ptr<const cdf_max_layer> layer_derived = nnforge_dynamic_pointer_cast<const cdf_max_layer>(layer_schema);
			const unsigned int entry_subsampling_size = layer_derived->entry_subsampling_size;
			const bool is_min = layer_derived->is_min;
			const int total_workload = entry_count;

			#pragma omp parallel default(none) num_threads(plain_config->openmp_thread_count)
			{
				#pragma omp for schedule(guided)
				for(int workload_id = 0; workload_id < total_workload; ++workload_id)
				{
					int entry_id = workload_id;

					const float * in_it_base = in_it_global + entry_id * neuron_count * entry_subsampling_size;
					float * out_it_base = out_it_global + entry_id * neuron_count;

					for(float * out_it = out_it_base; out_it != out_it_base + neuron_count; ++out_it, ++in_it_base)
					{
						if (is_min)
						{
							float product = 1.0F - *in_it_base;
							for(unsigned int i = 1; i < entry_subsampling_size; ++i)
								product *= (1.0F - *(in_it_base + neuron_count * i));

							*out_it = 1.0F - product;
						}
						else
						{
							float product = *in_it_base;
							for(unsigned int i = 1; i < entry_subsampling_size; ++i)
								product *= *(in_it_base + neuron_count * i);

							*out_it = product;
						}
					}
				}
			}
		}
	}
}
