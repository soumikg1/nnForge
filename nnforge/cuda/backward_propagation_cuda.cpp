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

#include "backward_propagation_cuda.h"

#include "layer_updater_schema_factory.h"
#include "cuda_linear_buffer_host.h"
#include "neural_network_cuda_exception.h"
#include "cuda_profiling.h"
#include "util_cuda.h"

#include "../data_layer.h"
#include "../neural_network_exception.h"

#include <boost/filesystem/fstream.hpp>
#include <boost/format.hpp>
#include <numeric>

namespace nnforge
{
	namespace cuda
	{
		bool backward_propagation_cuda::dump_data = false;

		const unsigned int backward_propagation_cuda::elem_count_update_accum_per_part = 64;
		backward_propagation_cuda::backward_propagation_cuda(
			const network_schema& schema,
			const std::vector<std::string>& output_layer_names,
			const std::vector<std::string>& error_source_layer_names,
			const std::vector<std::string>& exclude_data_update_layer_names,
			debug_state::ptr debug,
			profile_state::ptr profile,
			cuda_running_configuration::const_ptr cuda_config)
			: backward_propagation(schema, output_layer_names, error_source_layer_names, exclude_data_update_layer_names, debug, profile)
			, cuda_config(cuda_config)
		{
			cuda_config->set_device();

			std::vector<layer_name_with_action> actions = action_schema->get_actions();

			for(std::vector<layer_name_with_action>::const_iterator it = actions.begin(); it != actions.end(); ++it)
			{
				if (it->get_action().get_action_type() == layer_action::backward_data)
				{
					layer::const_ptr l = this->schema->get_layer(it->get_name());
					const std::string& previous_layer_name = l->input_layer_instance_names[it->get_action().get_backprop_index()];
					input_to_all_output_map.insert(std::make_pair(previous_layer_name, std::vector<layer_name_with_action>())).first->second.push_back(*it);
				}
				else if (it->get_action().get_action_type() == layer_action::backward_data_and_weights)
				{
					layer::const_ptr l = this->schema->get_layer(it->get_name());
					for(std::vector<std::string>::const_iterator it2 = l->input_layer_instance_names.begin(); it2 != l->input_layer_instance_names.end(); ++it2)
					{
						const std::string& previous_layer_name = *it2;
						input_to_all_output_map.insert(std::make_pair(previous_layer_name, std::vector<layer_name_with_action>())).first->second.push_back(*it);
					}
				}
			}

			setup_network_cuda();

			std::set<std::string> action_layer_names;
			for(std::vector<layer_name_with_action>::const_iterator it = actions.begin(); it != actions.end(); ++it)
				action_layer_names.insert(it->get_name());
			for(std::set<std::string>::const_iterator it = action_layer_names.begin(); it != action_layer_names.end(); ++it)
				updater_schemas.insert(
					std::make_pair(
						*it,
						layer_updater_schema_factory::singleton::get_const_instance().create_updater_schema_layer(this->schema->get_layer(*it), cuda_config)));

			for(std::map<std::string, layer_updater_schema::const_ptr>::const_iterator it = updater_schemas.begin(); it != updater_schemas.end(); ++it)
				schema_data.insert(std::make_pair(it->first, it->second->get_schema_buffers()));
		}

		backward_propagation_cuda::~backward_propagation_cuda()
		{
		}

		void backward_propagation_cuda::actual_run(
			structured_data_bunch_reader& reader,
			structured_data_bunch_writer& writer,
			network_data& data,
			network_data::ptr momentum_data,
			network_data::ptr momentum_data2,
			const std::map<std::string, std::vector<float> >& learning_rates,
			unsigned int batch_size,
			float weight_decay,
			training_momentum momentum,
			unsigned int epoch_id,
			std::map<std::string, std::vector<float> >& average_absolute_updates,
			unsigned int& entries_processed,
			std::map<layer_name_with_action, float>& action_seconds)
		{
			cuda_config->set_device();

			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> > net_data = get_data(data.data_list);
			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> > gradient = get_zero_gradient(net_data);
			std::map<std::string, std::vector<cuda_linear_buffer_device::const_ptr> > persistent_working_data;
			std::map<std::string, std::vector<cuda_linear_buffer_device::const_ptr> > net_data_custom;
			for(std::map<std::string, layer_updater_cuda::ptr>::const_iterator it = updaters.begin(); it != updaters.end(); ++it)
			{
				layer_data_custom::const_ptr dt_custom = data.data_custom_list.find(it->first);
				if (dt_custom)
					net_data_custom.insert(std::make_pair(it->first, it->second->set_get_data_custom(dt_custom)));
				persistent_working_data.insert(std::make_pair(it->first, it->second->get_persistent_working_data()));
			}

			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> > previous_upd;
			if (momentum.is_momentum_data())
				previous_upd = get_data(momentum_data->data_list);
			else
			{
				for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = net_data.begin(); it != net_data.end(); ++it)
					previous_upd.insert(std::make_pair(it->first, std::vector<cuda_linear_buffer_device::ptr>()));
			}

			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> > previous_upd2;
			if (momentum.is_momentum_data2())
				previous_upd2 = get_data(momentum_data2->data_list);
			else
			{
				for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = net_data.begin(); it != net_data.end(); ++it)
					previous_upd2.insert(std::make_pair(it->first, std::vector<cuda_linear_buffer_device::ptr>()));
			}

			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> > update_accum_buffers;
			for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = net_data.begin(); it != net_data.end(); ++it)
			{
				std::vector<cuda_linear_buffer_device::ptr>& dst_buffers = update_accum_buffers.insert(std::make_pair(it->first, std::vector<cuda_linear_buffer_device::ptr>())).first->second;
				for(unsigned int i = 0; i < static_cast<unsigned int>(it->second.size()); ++i)
				{
					dst_buffers.push_back(cuda_linear_buffer_device::ptr(new cuda_linear_buffer_device(elem_count_update_accum_per_part * sizeof(double))));
					cuda_util::set_with_value(
						*cuda_config,
						(double *)(*dst_buffers.back()),
						0.0,
						elem_count_update_accum_per_part,
						*copy_data_stream);
				}
			}

			buffer_cuda_size_configuration buffer_configuration = buffer_config_without_data_and_momentum;
			for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = net_data.begin(); it != net_data.end(); ++it)
				for(std::vector<cuda_linear_buffer_device::ptr>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2)
					buffer_configuration.add_constant_buffer((*it2)->get_size());
			for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = previous_upd.begin(); it != previous_upd.end(); ++it)
				for(std::vector<cuda_linear_buffer_device::ptr>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2)
					buffer_configuration.add_constant_buffer((*it2)->get_size());
			for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = previous_upd2.begin(); it != previous_upd2.end(); ++it)
				for(std::vector<cuda_linear_buffer_device::ptr>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2)
					buffer_configuration.add_constant_buffer((*it2)->get_size());
			for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = gradient.begin(); it != gradient.end(); ++it)
				for(std::vector<cuda_linear_buffer_device::ptr>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2)
					buffer_configuration.add_constant_buffer((*it2)->get_size());
			for(std::map<std::string, std::vector<cuda_linear_buffer_device::const_ptr> >::const_iterator it = net_data_custom.begin(); it != net_data_custom.end(); ++it)
				for(std::vector<cuda_linear_buffer_device::const_ptr>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2)
					buffer_configuration.add_constant_buffer((*it2)->get_size());
			for(std::map<std::string, std::vector<cuda_linear_buffer_device::const_ptr> >::const_iterator it = persistent_working_data.begin(); it != persistent_working_data.end(); ++it)
				for(std::vector<cuda_linear_buffer_device::const_ptr>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2)
					buffer_configuration.add_constant_buffer((*it2)->get_size());
			for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = update_accum_buffers.begin(); it != update_accum_buffers.end(); ++it)
				for(std::vector<cuda_linear_buffer_device::ptr>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2)
					buffer_configuration.add_constant_buffer((*it2)->get_size());

			unsigned int max_entry_count = cuda_config->get_max_entry_count(buffer_configuration);

			if (debug->is_debug())
			{
				std::stringstream debug_str;
				debug_str << "backward prop cuda max packet size: " << max_entry_count;
				debug->output_message(debug_str.str().c_str());
			}

			if (max_entry_count == 0)
				throw neural_network_exception("Insufficient memory to do forward-backward prop for even one sample");

			std::vector<unsigned int> entry_read_count_list;
			if (batch_size <= max_entry_count)
				entry_read_count_list.push_back(batch_size);
			else
			{
				unsigned int chunk_count = (batch_size + max_entry_count - 1) / max_entry_count;
				unsigned int chunk_min_size = batch_size / chunk_count;
				unsigned int plus1_chunk_count = batch_size % chunk_count;
				entry_read_count_list.resize(chunk_count);
				std::fill_n(entry_read_count_list.begin(), plus1_chunk_count, chunk_min_size + 1);
				std::fill_n(entry_read_count_list.begin() + plus1_chunk_count, chunk_count - plus1_chunk_count, chunk_min_size);

				if (debug->is_debug())
				{
					std::stringstream debug_str;
					debug_str << "Batch " << batch_size << " is split into multiple chunks: ";
					for(std::vector<unsigned int>::const_iterator it = entry_read_count_list.begin(); it != entry_read_count_list.end(); ++it)
					{
						if (it != entry_read_count_list.begin())
							debug_str << ", ";
						debug_str << *it;
					}
					debug->output_message(debug_str.str().c_str());
				}
			}
			unsigned int max_chunk_size = *std::max_element(entry_read_count_list.begin(), entry_read_count_list.end());

			std::map<std::string, nnforge_array<cuda_linear_buffer_device::ptr, 2> > dedicated_buffers;
			for(std::map<std::string, size_t>::const_iterator it = dedicated_per_entry_data_name_to_size_map.begin(); it != dedicated_per_entry_data_name_to_size_map.end(); ++it)
			{
				nnforge_array<cuda_linear_buffer_device::ptr, 2>& arr = dedicated_buffers.insert(std::make_pair(it->first, nnforge_array<cuda_linear_buffer_device::ptr, 2>())).first->second;
				arr[0] = cuda_linear_buffer_device::ptr(new cuda_linear_buffer_device(it->second * max_chunk_size));
				arr[1] = cuda_linear_buffer_device::ptr(new cuda_linear_buffer_device(it->second * max_chunk_size));
			}

			std::map<std::string, cuda_linear_buffer_host::ptr> input_host_buffers;
			for(std::map<std::string, size_t>::const_iterator it = input_per_entry_host_data_name_to_size_map.begin(); it != input_per_entry_host_data_name_to_size_map.end(); ++it)
				input_host_buffers.insert(std::make_pair(it->first,
					cuda_linear_buffer_host::ptr(new cuda_linear_buffer_host(it->second * max_chunk_size))));
			std::map<std::string, cuda_linear_buffer_host::ptr> output_host_buffers;
			for(std::map<std::string, size_t>::const_iterator it = output_per_entry_host_data_name_to_size_map.begin(); it != output_per_entry_host_data_name_to_size_map.end(); ++it)
				output_host_buffers.insert(std::make_pair(it->first,
					cuda_linear_buffer_host::ptr(new cuda_linear_buffer_host(it->second * max_chunk_size))));
	
			run_kernels_task_ready = false;

			unsigned int entry_processed_count = 0;
			unsigned int chunk_index = 0;

			cuda_safe_call(cudaStreamSynchronize(*copy_data_stream));

			unsigned int base_iteration_count = 0;
			if (momentum.type == training_momentum::adam_momentum)
			{
				int epoch_entry_count = reader.get_entry_count();
				if (epoch_entry_count >= 0)
					base_iteration_count = epoch_id * ((epoch_entry_count + batch_size - 1) / batch_size);
				else
					throw neural_network_exception("Training data reader doesn't report entry_count, which is required for ADAM momentum");
			}

			run_kernels_params params(
				dedicated_buffers,
				net_data,
				net_data_custom,
				persistent_working_data,
				gradient,
				previous_upd,
				previous_upd2,
				update_accum_buffers,
				learning_rates,
				batch_size,
				weight_decay,
				momentum,
				max_chunk_size,
				base_iteration_count);
			boost::thread run_kernels_thread(run_kernels_static, this, &params);
			try
			{
				run_kernels_thread_io_set = 0;
				bool initial_iteration = true;
				bool try_to_read = true;
				bool run_kernels_thread_stopped = false;
				bool entry_not_read_encountered = false;
				unsigned int entry_to_process_count = 0;
				unsigned int entry_to_write_count = 0;
				unsigned int base_entry_to_read_id = 0;
				std::vector<read_entry_info::ptr> read_entry_info_list(entry_read_count_list[chunk_index]);
				for(unsigned int i = 0; i < entry_read_count_list[chunk_index]; ++i)
				{
					read_entry_info_list[i] = read_entry_info::ptr(new read_entry_info());
					read_entry_info_list[i]->reader = &reader;
					for(std::map<std::string, size_t>::const_iterator it = input_per_entry_host_data_name_to_size_map.begin(); it != input_per_entry_host_data_name_to_size_map.end(); ++it)
						read_entry_info_list[i]->data_map.insert(std::make_pair(it->first, (float *)(*input_host_buffers[it->first]) + i * (it->second / sizeof(float))));
				}

				while(true)
				{
					unsigned int copy_data_thread_io_set = 1 - run_kernels_thread_io_set;
					bool wait_for_kernels_to_finish = false;
					if (!initial_iteration && !run_kernels_thread_stopped)
					{
						// Set command
						run_kernels_thread_entry_to_process_count = entry_to_process_count;
						run_kernels_finished = false;
						{
							boost::lock_guard<boost::mutex> lock(run_kernels_pending_mutex);
							run_kernels_task_ready = true;
						}
						run_kernels_pending_condition.notify_one();
						run_kernels_thread_stopped = (run_kernels_thread_entry_to_process_count == 0);
						wait_for_kernels_to_finish = !run_kernels_thread_stopped;
					}

					// Launch D2H copy for output data
					if (entry_to_write_count > 0)
					{
						for(std::map<std::string, cuda_linear_buffer_host::ptr>::iterator it = output_host_buffers.begin(); it != output_host_buffers.end(); ++it)
						{
							cuda_safe_call(cudaMemcpyAsync(
								*it->second,
								*dedicated_buffers[it->first][copy_data_thread_io_set],
								output_per_entry_host_data_name_to_size_map[it->first] * entry_to_write_count,
								cudaMemcpyDeviceToHost,
								*copy_data_stream));
						}
						if (cuda_config->is_flush_required())
							cuda_relaxed_safe_call(cudaStreamQuery(*copy_data_stream));
					}

					unsigned int entry_read_count = 0;
					if (!entry_not_read_encountered)
					{
						PUSH_RANGE("Reading input data", 0);
						// Launch all read input data tasks
						for(unsigned int i = 0; i < entry_read_count_list[chunk_index]; ++i)
						{
							read_entry_info& current_info = *read_entry_info_list[i];
							current_info.read_entry_finished = false;
							current_info.entry_id = base_entry_to_read_id + i;
							cuda_config->get_job_runner()->service.post(boost::bind(read_input_data_static, &current_info));
						}

						// Wait for all input data to be read
						for(unsigned int i = 0; i < entry_read_count_list[chunk_index]; ++i)
						{
							read_entry_info& current_info = *read_entry_info_list[i];

							{
								boost::unique_lock<boost::mutex> lock(current_info.read_entry_finished_mutex);
								while (!current_info.read_entry_finished)
									current_info.read_entry_finished_condition.wait(lock);
							}
							if (!current_info.error_message.empty())
							{
								for(unsigned int j = i; j < entry_read_count_list[chunk_index]; ++j)
								{
									read_entry_info& current_info = *read_entry_info_list[j];
									{
										boost::unique_lock<boost::mutex> lock(current_info.read_entry_finished_mutex);
										while (!current_info.read_entry_finished)
											current_info.read_entry_finished_condition.wait(lock);
									}
								}
								throw neural_network_exception(params.error_message);
							}
							if (!entry_not_read_encountered)
							{
								if (current_info.entry_read)
									++entry_read_count;
								else
									entry_not_read_encountered = true;
							}
						}
						POP_RANGE;
					} // if (!entry_not_read_encountered)

					// Make sure output data is copied to host
					cuda_safe_call(cudaStreamSynchronize(*copy_data_stream));

					// Launch H2D copy for input data
					if (entry_read_count > 0)
					{
						for(std::map<std::string, cuda_linear_buffer_host::ptr>::iterator it = input_host_buffers.begin(); it != input_host_buffers.end(); ++it)
						{
							cuda_safe_call(cudaMemcpyAsync(
								*dedicated_buffers[it->first][copy_data_thread_io_set],
								*it->second,
								input_per_entry_host_data_name_to_size_map[it->first] * entry_read_count,
								cudaMemcpyDeviceToHost,
								*copy_data_stream));
						}
						if (cuda_config->is_flush_required())
							cuda_relaxed_safe_call(cudaStreamQuery(*copy_data_stream));
					}

					// Write output data
					if (entry_to_write_count > 0)
					{
						PUSH_RANGE("Writing output data", 1);
						for(unsigned int i = 0; i < entry_to_write_count * output_layers_tiling_factor; ++i)
						{
							std::map<std::string, const float *> data_map;
							for(std::map<std::string, size_t>::const_iterator it = output_per_entry_host_data_name_to_size_map.begin(); it != output_per_entry_host_data_name_to_size_map.end(); ++it)
								data_map.insert(std::make_pair(it->first, (float *)(*output_host_buffers[it->first]) + i * (it->second / sizeof(float) / output_layers_tiling_factor)));
							writer.write(entry_processed_count + i, data_map);
						}
						POP_RANGE;
					}

					// Make sure input data is copied to device
					cuda_safe_call(cudaStreamSynchronize(*copy_data_stream));

					if (wait_for_kernels_to_finish)
					{
						PUSH_RANGE("Waiting for kernels to finish", 2);
						// Wait for all the kernels to finish execution
						{
							boost::unique_lock<boost::mutex> lock(run_kernels_finished_mutex);
							while (!run_kernels_finished)
								run_kernels_finished_condition.wait(lock);
						}
						POP_RANGE;
						if (!params.error_message.empty())
							throw neural_network_exception(params.error_message);
					}

					run_kernels_thread_io_set = 1 - run_kernels_thread_io_set; // Switch set of IO buffers
					initial_iteration = false;
					entry_processed_count += entry_to_write_count;
					base_entry_to_read_id += entry_read_count;
					entry_to_write_count = entry_to_process_count;
					entry_to_process_count = entry_read_count;
					chunk_index = (chunk_index + 1) % entry_read_count_list.size();

					if ((entry_read_count == 0) && (!wait_for_kernels_to_finish))
						break;
				}
			}
			catch (const std::exception&)
			{
				run_kernels_thread.interrupt();
				run_kernels_thread.join();
				throw;
			}

			run_kernels_thread.join();
			if (!params.error_message.empty())
				throw neural_network_exception(params.error_message);

			read_data(net_data, data.data_list);

			if (momentum.is_momentum_data())
				read_data(previous_upd, momentum_data->data_list);
			if (momentum.is_momentum_data2())
				read_data(previous_upd2, momentum_data2->data_list);

			entries_processed = entry_processed_count;
			average_absolute_updates = read_update_accum(
				update_accum_buffers,
				net_data,
				params.gradient_applied_count);
			action_seconds.clear();
			for(std::map<layer_name_with_action, double>::const_iterator it = params.action_seconds.begin(); it != params.action_seconds.end(); ++it)
				action_seconds.insert(std::make_pair(it->first, static_cast<float>(it->second)));
		}

		void backward_propagation_cuda::run_kernels(run_kernels_params& params)
		{
			try
			{
				cuda_config->set_device();

				std::vector<cuda_linear_buffer_device::ptr> fixed_buffers;
				for(std::vector<size_t>::const_iterator it = fixed_set_size_list.begin(); it != fixed_set_size_list.end(); ++it)
					fixed_buffers.push_back(cuda_linear_buffer_device::ptr(new cuda_linear_buffer_device(*it)));

				std::vector<cuda_linear_buffer_device::ptr> layer_buffers;
				for(std::vector<size_t>::const_iterator it = layer_buffer_set_per_entry_size_list.begin(); it != layer_buffer_set_per_entry_size_list.end(); ++it)
					layer_buffers.push_back(cuda_linear_buffer_device::ptr(new cuda_linear_buffer_device(*it * params.max_chunk_size)));

				unsigned int gradient_accumulated_entry_count = 0;

				boost::unique_lock<boost::mutex> lock(run_kernels_pending_mutex);
				while(true)
				{
					boost::this_thread::interruption_point();

					while (!run_kernels_task_ready)
						run_kernels_pending_condition.wait(lock);

					run_kernels_task_ready = false;

					gradient_accumulated_entry_count += run_kernels_thread_entry_to_process_count;
					float gradient_normalizer = 0.0F;
					bool apply_gradient = false;
					if (gradient_accumulated_entry_count >= params.batch_size)
					{
						gradient_normalizer = 1.0F / static_cast<float>(gradient_accumulated_entry_count);
						apply_gradient = true;
					}
					else if ((run_kernels_thread_entry_to_process_count == 0) && (gradient_accumulated_entry_count > 0))
					{
						gradient_normalizer = 1.0F / static_cast<float>(params.batch_size);
						apply_gradient = true;
					}
					if (apply_gradient)
					{
						gradient_accumulated_entry_count = 0;
						params.gradient_applied_count++;
					}

					std::set<layer_name_with_action> actions_profiled;

					if (run_kernels_thread_entry_to_process_count == 0)
					{
						// Apply remaining gradients and then exit main loop
						if (apply_gradient)
						{
							for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = params.update_accum_buffers.begin(); it != params.update_accum_buffers.end(); ++it)
							{
								const std::string& layer_name = it->first;
								layer_name_with_action current_layer_name_with_action = layer_name_with_action(layer_name, layer_action(layer_action::update_weights));

								if (profile->is_profile())
								{
									cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[current_layer_name_with_action].first, *command_streams[output_data_ready_stream_set_id]));
									actions_profiled.insert(current_layer_name_with_action);
								}

								enqueue_apply_gradient(
									*command_streams[output_data_ready_stream_set_id],
									layer_name,
									params.net_data[layer_name],
									params.gradient[layer_name],
									params.previous_upd[layer_name],
									params.previous_upd2[layer_name],
									params.learning_rates.find(layer_name)->second,
									params.update_accum_buffers[layer_name],
									gradient_normalizer,
									params.weight_decay,
									params.momentum,
									params.base_iteration_count + params.gradient_applied_count);

								if (profile->is_profile())
									cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[layer_name_with_action(layer_name, layer_action(layer_action::update_weights))].second, *command_streams[output_data_ready_stream_set_id]));
							}
						}
						break;
					}

					for(std::vector<layer_name_with_action>::const_iterator action_it = actions_in_execution_order.begin(); action_it  != actions_in_execution_order.end(); ++action_it)
					{
						const layer_name_with_action& current_layer_name_with_action = *action_it;
						std::string layer_name = current_layer_name_with_action.get_name();;
						layer_action action = current_layer_name_with_action.get_action();
						layer::const_ptr current_layer = schema->find_layer(layer_name);
						unsigned int tiling_factor = cumulative_tiling_factor_map[layer_name];

						cuda_stream::ptr current_stream = command_streams[action_to_stream_set_map[current_layer_name_with_action]];

						// Enqueue waits for previous events
						{
							std::map<layer_name_with_action, std::vector<cuda_event::ptr> >::const_iterator previous_events_it = action_previous_events.find(current_layer_name_with_action);
							if (previous_events_it != action_previous_events.end())
							{
								const std::vector<cuda_event::ptr>& previous_events = previous_events_it->second;
								for(std::vector<cuda_event::ptr>::const_iterator event_it = previous_events.begin(); event_it != previous_events.end(); ++event_it)
									cuda_safe_call(cudaStreamWaitEvent(*current_stream, **event_it, 0));
							}
						}

						// Enqueue action
						{
							cuda_linear_buffer_device::ptr temporary_working_fixed_buffer;
							{
								std::map<layer_name_with_action, unsigned int>::const_iterator it = temporary_working_fixed_data_action_to_set_map.find(current_layer_name_with_action);
								if (it != temporary_working_fixed_data_action_to_set_map.end())
									temporary_working_fixed_buffer = fixed_buffers[it->second];
							}

							cuda_linear_buffer_device::ptr temporary_working_per_entry_buffer;
							{
								std::map<layer_name_with_action, unsigned int>::const_iterator it = temporary_working_per_entry_data_action_to_set_map.find(current_layer_name_with_action);
								if (it != temporary_working_per_entry_data_action_to_set_map.end())
									temporary_working_per_entry_buffer = layer_buffers[it->second];
							}

							std::vector<cuda_linear_buffer_device::const_ptr> data_custom_list;
							{
								std::map<std::string, std::vector<cuda_linear_buffer_device::const_ptr> >::const_iterator data_custom_list_it = params.net_data_custom.find(layer_name);
								if (data_custom_list_it != params.net_data_custom.end())
									data_custom_list = data_custom_list_it->second;
							}

							switch (action.get_action_type())
							{
							case layer_action::forward:
								{
									cuda_linear_buffer_device::ptr output_buffer;
									{
										std::map<layer_name_with_action, unsigned int>::const_iterator it = layer_buffer_action_to_set_map.find(current_layer_name_with_action);
										if (it != layer_buffer_action_to_set_map.end())
											output_buffer = layer_buffers[it->second];
										else
											output_buffer = params.dedicated_buffers.find(layer_name)->second[run_kernels_thread_io_set];
									}

									std::vector<cuda_linear_buffer_device::const_ptr> input_buffers;
									for(std::vector<std::string>::const_iterator input_layer_name_it = current_layer->input_layer_instance_names.begin(); input_layer_name_it != current_layer->input_layer_instance_names.end(); ++input_layer_name_it)
									{
										std::map<layer_name_with_action, unsigned int>::const_iterator it = layer_buffer_action_to_set_map.find(layer_name_with_action(*input_layer_name_it, layer_action::forward));
										if (it != layer_buffer_action_to_set_map.end())
											input_buffers.push_back(layer_buffers[it->second]);
										else
											input_buffers.push_back(params.dedicated_buffers.find(*input_layer_name_it)->second[run_kernels_thread_io_set]);
										if (dump_data)
											cuda_util::dump_list(
												(const float *)*input_buffers.back(),
												layer_config_map[*input_layer_name_it].get_neuron_count() * run_kernels_thread_entry_to_process_count * cumulative_tiling_factor_map[*input_layer_name_it],
												(std::string("debug_") + current_layer_name_with_action.get_name() + "_" + current_layer_name_with_action.get_action().str() + "_input_buffers_" + *input_layer_name_it + ".txt").c_str(),
												*current_stream);
									}

									cuda_linear_buffer_device::ptr temporary_per_entry_buffer;
									{
										std::map<layer_name_with_action, unsigned int>::const_iterator it = temporary_per_entry_data_action_to_set_map.find(current_layer_name_with_action);
										if (it != temporary_per_entry_data_action_to_set_map.end())
											temporary_per_entry_buffer = layer_buffers[it->second];
									}

									cuda_linear_buffer_device::ptr temporary_fixed_buffer;
									{
										std::map<layer_name_with_action, unsigned int>::const_iterator it = temporary_fixed_data_action_to_set_map.find(current_layer_name_with_action);
										if (it != temporary_fixed_data_action_to_set_map.end())
											temporary_fixed_buffer = fixed_buffers[it->second];
									}

									std::vector<cuda_linear_buffer_device::const_ptr> data_list;
									{
										std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator data_list_it = params.net_data.find(layer_name);
										if (data_list_it != params.net_data.end())
											data_list.insert(data_list.end(), data_list_it->second.begin(), data_list_it->second.end());
									}

									if (profile->is_profile())
									{
										cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[current_layer_name_with_action].first, *current_stream));
										actions_profiled.insert(current_layer_name_with_action);
									}

									updaters.find(layer_name)->second->enqueue_forward_propagation(
										*current_stream,
										output_buffer,
										schema_data[layer_name],
										data_list,
										data_custom_list,
										input_buffers,
										params.persistent_working_data[layer_name],
										temporary_working_fixed_buffer,
										temporary_working_per_entry_buffer,
										temporary_fixed_buffer,
										temporary_per_entry_buffer,
										run_kernels_thread_entry_to_process_count * tiling_factor);

									if (dump_data)
										cuda_util::dump_list(
											(const float *)*output_buffer,
											layer_config_map[layer_name].get_neuron_count() * run_kernels_thread_entry_to_process_count * tiling_factor,
											(std::string("debug_") + current_layer_name_with_action.get_name() + "_" + current_layer_name_with_action.get_action().str() + "_output_buffer" + ".txt").c_str(),
											*current_stream);

									if (profile->is_profile())
										cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[current_layer_name_with_action].second, *current_stream));
								}
								break;
							case layer_action::backward_data:
								{
									cuda_linear_buffer_device::ptr output_buffer = layer_buffers[layer_buffer_action_to_set_map[current_layer_name_with_action]];

									std::vector<cuda_linear_buffer_device::const_ptr> input_neurons_buffers;
									unsigned int data_input_index = 0;
									for(std::vector<std::string>::const_iterator input_layer_name_it = current_layer->input_layer_instance_names.begin(); input_layer_name_it != current_layer->input_layer_instance_names.end(); ++input_layer_name_it, ++data_input_index)
									{
										if (updaters[layer_name]->is_backward_data_dependent_on_input_buffer(action.get_backprop_index(), data_input_index))
										{
											std::map<layer_name_with_action, unsigned int>::const_iterator it = layer_buffer_action_to_set_map.find(layer_name_with_action(*input_layer_name_it, layer_action::forward));
											if (it != layer_buffer_action_to_set_map.end())
												input_neurons_buffers.push_back(layer_buffers[it->second]);
											else
												input_neurons_buffers.push_back(params.dedicated_buffers.find(*input_layer_name_it)->second[run_kernels_thread_io_set]);
										}
										else
											input_neurons_buffers.push_back(cuda_linear_buffer_device::const_ptr());
									}

									cuda_linear_buffer_device::ptr temporary_per_entry_buffer;
									{
										if (updaters[layer_name]->is_backward_data_dependent_on_temporary_per_entry_buffer(action.get_backprop_index()))
										{
											std::map<layer_name_with_action, unsigned int>::const_iterator it = temporary_per_entry_data_action_to_set_map.find(layer_name_with_action(layer_name, layer_action::forward));
											if (it != temporary_per_entry_data_action_to_set_map.end())
												temporary_per_entry_buffer = layer_buffers[it->second];
										}
									}

									cuda_linear_buffer_device::ptr temporary_fixed_buffer;
									{
										if (updaters[layer_name]->is_backward_data_dependent_on_temporary_fixed_buffer(action.get_backprop_index()))
										{
											std::map<layer_name_with_action, unsigned int>::const_iterator it = temporary_fixed_data_action_to_set_map.find(layer_name_with_action(layer_name, layer_action::forward));
											if (it != temporary_fixed_data_action_to_set_map.end())
												temporary_fixed_buffer = fixed_buffers[it->second];
										}
									}

									cuda_linear_buffer_device::const_ptr output_neurons_buffer;
									{
										if (updaters[layer_name]->is_backward_data_dependent_on_output_buffer(action.get_backprop_index()))
										{
											std::map<layer_name_with_action, unsigned int>::const_iterator it = layer_buffer_action_to_set_map.find(layer_name_with_action(layer_name, layer_action::forward));
											if (it != layer_buffer_action_to_set_map.end())
												output_neurons_buffer = layer_buffers[it->second];
											else
												output_neurons_buffer = params.dedicated_buffers.find(layer_name)->second[run_kernels_thread_io_set];
										}
									}

									cuda_linear_buffer_device::const_ptr output_errors_buffer;
									{
										std::map<std::string, std::vector<layer_name_with_action> >::const_iterator it = input_to_all_output_map.find(layer_name);
										if (it != input_to_all_output_map.end())
										{
											output_errors_buffer = layer_buffers[layer_buffer_action_to_set_map[it->second.front()]];
											if (dump_data)
												cuda_util::dump_list(
													(const float *)*output_errors_buffer,
													layer_config_map[layer_name].get_neuron_count() * run_kernels_thread_entry_to_process_count * tiling_factor,
													(std::string("debug_") + current_layer_name_with_action.get_name() + "_" + current_layer_name_with_action.get_action().str() + "_output_errors" + ".txt").c_str(),
													*current_stream);
										}
									}

									std::vector<cuda_linear_buffer_device::const_ptr> data_list;
									{
										std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator data_list_it = params.net_data.find(layer_name);
										if (data_list_it != params.net_data.end())
											data_list.insert(data_list.end(), data_list_it->second.begin(), data_list_it->second.end());
									}

									if (profile->is_profile())
									{
										cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[current_layer_name_with_action].first, *current_stream));
										actions_profiled.insert(current_layer_name_with_action);
									}

									if ((dump_data) && (add_output_actions.find(current_layer_name_with_action) != add_output_actions.end()))
										cuda_util::dump_list(
											(const float *)*output_buffer,
											layer_config_map[current_layer->input_layer_instance_names[current_layer_name_with_action.get_action().get_backprop_index()]].get_neuron_count() * run_kernels_thread_entry_to_process_count * tiling_factor,
											(std::string("debug_") + current_layer_name_with_action.get_name() + "_" + current_layer_name_with_action.get_action().str() + "_original_input_errors" + ".txt").c_str(),
											*current_stream);

									updaters.find(layer_name)->second->enqueue_backward_data_propagation(
										*current_stream,
										action.get_backprop_index(),
										output_buffer,
										output_errors_buffer,
										schema_data[layer_name],
										data_list,
										data_custom_list,
										input_neurons_buffers,
										output_neurons_buffer,
										params.persistent_working_data[layer_name],
										temporary_working_fixed_buffer,
										temporary_working_per_entry_buffer,
										temporary_fixed_buffer,
										temporary_per_entry_buffer,
										add_output_actions.find(current_layer_name_with_action) != add_output_actions.end(),
										run_kernels_thread_entry_to_process_count * tiling_factor);

									if (dump_data)
										cuda_util::dump_list(
											(const float *)*output_buffer,
											layer_config_map[current_layer->input_layer_instance_names[current_layer_name_with_action.get_action().get_backprop_index()]].get_neuron_count() * run_kernels_thread_entry_to_process_count * tiling_factor,
											(std::string("debug_") + current_layer_name_with_action.get_name() + "_" + current_layer_name_with_action.get_action().str() + "_input_errors" + ".txt").c_str(),
											*current_stream);

									if (profile->is_profile())
										cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[current_layer_name_with_action].second, *current_stream));
								}
								break;
							case layer_action::backward_data_and_weights:
								{
									std::vector<cuda_linear_buffer_device::ptr> output_buffers(1, layer_buffers[layer_buffer_action_to_set_map[current_layer_name_with_action]]);

									std::vector<cuda_linear_buffer_device::const_ptr> input_neurons_buffers;
									unsigned int data_input_index = 0;
									for(std::vector<std::string>::const_iterator input_layer_name_it = current_layer->input_layer_instance_names.begin(); input_layer_name_it != current_layer->input_layer_instance_names.end(); ++input_layer_name_it, ++data_input_index)
									{
										if (updaters[layer_name]->is_backward_data_and_weights_dependent_on_input_buffer(data_input_index))
										{
											std::map<layer_name_with_action, unsigned int>::const_iterator it = layer_buffer_action_to_set_map.find(layer_name_with_action(*input_layer_name_it, layer_action::forward));
											if (it != layer_buffer_action_to_set_map.end())
												input_neurons_buffers.push_back(layer_buffers[it->second]);
											else
												input_neurons_buffers.push_back(params.dedicated_buffers.find(*input_layer_name_it)->second[run_kernels_thread_io_set]);
										}
										else
											input_neurons_buffers.push_back(cuda_linear_buffer_device::const_ptr());
									}

									cuda_linear_buffer_device::ptr temporary_per_entry_buffer;
									{
										if (updaters[layer_name]->is_backward_data_and_weights_dependent_on_temporary_per_entry_buffer())
										{
											std::map<layer_name_with_action, unsigned int>::const_iterator it = temporary_per_entry_data_action_to_set_map.find(layer_name_with_action(layer_name, layer_action::forward));
											if (it != temporary_per_entry_data_action_to_set_map.end())
												temporary_per_entry_buffer = layer_buffers[it->second];
										}
									}

									cuda_linear_buffer_device::ptr temporary_fixed_buffer;
									{
										if (updaters[layer_name]->is_backward_data_and_weights_dependent_on_temporary_fixed_buffer())
										{
											std::map<layer_name_with_action, unsigned int>::const_iterator it = temporary_fixed_data_action_to_set_map.find(layer_name_with_action(layer_name, layer_action::forward));
											if (it != temporary_fixed_data_action_to_set_map.end())
												temporary_fixed_buffer = fixed_buffers[it->second];
										}
									}

									cuda_linear_buffer_device::const_ptr output_neurons_buffer;
									{
										if (updaters[layer_name]->is_backward_data_and_weights_dependent_on_output_buffer())
										{
											std::map<layer_name_with_action, unsigned int>::const_iterator it = layer_buffer_action_to_set_map.find(layer_name_with_action(layer_name, layer_action::forward));
											if (it != layer_buffer_action_to_set_map.end())
												output_neurons_buffer = layer_buffers[it->second];
											else
												output_neurons_buffer = params.dedicated_buffers.find(layer_name)->second[run_kernels_thread_io_set];
										}
									}

									cuda_linear_buffer_device::const_ptr output_errors_buffer;
									{
										std::map<std::string, std::vector<layer_name_with_action> >::const_iterator it = input_to_all_output_map.find(layer_name);
										if (it != input_to_all_output_map.end())
											output_errors_buffer = layer_buffers[layer_buffer_action_to_set_map[it->second.front()]];
									}

									std::vector<cuda_linear_buffer_device::const_ptr> data_list;
									{
										std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator data_list_it = params.net_data.find(layer_name);
										if (data_list_it != params.net_data.end())
											data_list.insert(data_list.end(), data_list_it->second.begin(), data_list_it->second.end());
									}

									if (profile->is_profile())
									{
										cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[current_layer_name_with_action].first, *current_stream));
										actions_profiled.insert(current_layer_name_with_action);
									}

									updaters.find(layer_name)->second->enqueue_backward_data_and_weights_propagation(
										*current_stream,
										output_buffers,
										output_errors_buffer,
										schema_data[layer_name],
										params.gradient[layer_name],
										data_list,
										data_custom_list,
										input_neurons_buffers,
										output_neurons_buffer,
										params.persistent_working_data[layer_name],
										temporary_working_fixed_buffer,
										temporary_working_per_entry_buffer,
										temporary_fixed_buffer,
										temporary_per_entry_buffer,
										add_output_actions.find(current_layer_name_with_action) != add_output_actions.end(),
										run_kernels_thread_entry_to_process_count * tiling_factor);

									if (profile->is_profile())
										cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[current_layer_name_with_action].second, *current_stream));
								}
								break;
							case layer_action::backward_weights:
								{
									std::vector<cuda_linear_buffer_device::const_ptr> input_neurons_buffers;
									unsigned int data_input_index = 0;
									for(std::vector<std::string>::const_iterator input_layer_name_it = current_layer->input_layer_instance_names.begin(); input_layer_name_it != current_layer->input_layer_instance_names.end(); ++input_layer_name_it, ++data_input_index)
									{
										if (updaters[layer_name]->is_backward_weights_dependent_on_input_buffer(data_input_index))
										{
											std::map<layer_name_with_action, unsigned int>::const_iterator it = layer_buffer_action_to_set_map.find(layer_name_with_action(*input_layer_name_it, layer_action::forward));
											if (it != layer_buffer_action_to_set_map.end())
												input_neurons_buffers.push_back(layer_buffers[it->second]);
											else
												input_neurons_buffers.push_back(params.dedicated_buffers.find(*input_layer_name_it)->second[run_kernels_thread_io_set]);
										}
										else
											input_neurons_buffers.push_back(cuda_linear_buffer_device::const_ptr());
									}

									cuda_linear_buffer_device::ptr temporary_per_entry_buffer;
									{
										if (updaters[layer_name]->is_backward_weights_dependent_on_temporary_per_entry_buffer())
										{
											std::map<layer_name_with_action, unsigned int>::const_iterator it = temporary_per_entry_data_action_to_set_map.find(layer_name_with_action(layer_name, layer_action::forward));
											if (it != temporary_per_entry_data_action_to_set_map.end())
												temporary_per_entry_buffer = layer_buffers[it->second];
										}
									}

									cuda_linear_buffer_device::ptr temporary_fixed_buffer;
									{
										if (updaters[layer_name]->is_backward_weights_dependent_on_temporary_fixed_buffer())
										{
											std::map<layer_name_with_action, unsigned int>::const_iterator it = temporary_fixed_data_action_to_set_map.find(layer_name_with_action(layer_name, layer_action::forward));
											if (it != temporary_fixed_data_action_to_set_map.end())
												temporary_fixed_buffer = fixed_buffers[it->second];
										}
									}

									cuda_linear_buffer_device::const_ptr output_errors_buffer;
									{
										std::map<std::string, std::vector<layer_name_with_action> >::const_iterator it = input_to_all_output_map.find(layer_name);
										if (it != input_to_all_output_map.end())
											output_errors_buffer = layer_buffers[layer_buffer_action_to_set_map[it->second.front()]];
									}

									if (profile->is_profile())
									{
										cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[current_layer_name_with_action].first, *current_stream));
										actions_profiled.insert(current_layer_name_with_action);
									}

									updaters.find(layer_name)->second->enqueue_backward_weights_propagation(
										*current_stream,
										schema_data[layer_name],
										params.gradient[layer_name],
										data_custom_list,
										input_neurons_buffers,
										output_errors_buffer,
										params.persistent_working_data[layer_name],
										temporary_working_fixed_buffer,
										temporary_working_per_entry_buffer,
										temporary_fixed_buffer,
										temporary_per_entry_buffer,
										run_kernels_thread_entry_to_process_count * tiling_factor);

									if (profile->is_profile())
										cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[current_layer_name_with_action].second, *current_stream));
								}
								break;
							case layer_action::update_weights:
								{
									if (apply_gradient)
									{
										if (profile->is_profile())
										{
											cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[current_layer_name_with_action].first, *current_stream));
											actions_profiled.insert(current_layer_name_with_action);
										}

										enqueue_apply_gradient(
											*current_stream,
											layer_name,
											params.net_data[layer_name],
											params.gradient[layer_name],
											params.previous_upd[layer_name],
											params.previous_upd2[layer_name],
											params.learning_rates.find(layer_name)->second,
											params.update_accum_buffers[layer_name],
											gradient_normalizer,
											params.weight_decay,
											params.momentum,
											params.base_iteration_count + params.gradient_applied_count);

										if (profile->is_profile())
											cuda_safe_call(cudaEventRecord(*start_stop_profiling_events[current_layer_name_with_action].second, *current_stream));
									}
								}
								break;
							}
						}

						// Enqeue event
						{
							std::map<layer_name_with_action, cuda_event::ptr>::const_iterator current_event_it = action_output_data_ready_events.find(current_layer_name_with_action);
							if (current_event_it != action_output_data_ready_events.end())
								cudaEventRecord(*current_event_it->second, *current_stream);
						}

						if (cuda_config->is_flush_required())
							cuda_relaxed_safe_call(cudaStreamQuery(*current_stream));
					}

					// Wait for target data to be ready
					for(std::vector<cuda_event::ptr>::const_iterator event_it = output_data_ready_additional_events.begin(); event_it != output_data_ready_additional_events.end(); ++event_it)
						cuda_safe_call(cudaStreamWaitEvent(*command_streams[output_data_ready_stream_set_id], **event_it, 0));

					// Wait for all kernels to finish
					cudaStreamSynchronize(*command_streams[output_data_ready_stream_set_id]);
		
					if (profile->is_profile())
					{
						for(std::set<layer_name_with_action>::const_iterator it_pr = actions_profiled.begin(); it_pr != actions_profiled.end(); ++it_pr)
						{
							std::map<layer_name_with_action, std::pair<cuda_event::ptr, cuda_event::ptr> >::const_iterator it = start_stop_profiling_events.find(*it_pr);
							float milliseconds;
							cuda_safe_call(cudaEventElapsedTime(&milliseconds, *it->second.first, *it->second.second));
							params.action_seconds.insert(std::make_pair(it->first, 0.0)).first->second += static_cast<double>(milliseconds * 0.001F);
						}
					}

					dump_data = false;

					// Notify caller thread that result is ready
					{
						boost::lock_guard<boost::mutex> lock(run_kernels_finished_mutex);
						run_kernels_finished = true;
					}
					run_kernels_finished_condition.notify_one();
				}
			}
			catch (const std::runtime_error& e)
			{
				params.error_message = e.what();
				{
					boost::lock_guard<boost::mutex> lock(run_kernels_finished_mutex);
					run_kernels_finished = true;
				}
				run_kernels_finished_condition.notify_one();
			}
		}

		backward_propagation_cuda::read_entry_info::read_entry_info()
		{
		}

		void backward_propagation_cuda::read_input_data_static(read_entry_info * params)
		{
			try
			{
				params->entry_read = params->reader->read(params->entry_id, params->data_map);

				// Notify caller thread that result is ready
				{
					boost::lock_guard<boost::mutex> lock(params->read_entry_finished_mutex);
					params->read_entry_finished = true;
				}
				params->read_entry_finished_condition.notify_one();
			}
			catch (const std::runtime_error& e)
			{
				params->error_message = e.what();
				{
					boost::lock_guard<boost::mutex> lock(params->read_entry_finished_mutex);
					params->read_entry_finished = true;
				}
				params->read_entry_finished_condition.notify_one();
			}
		}

		void backward_propagation_cuda::run_kernels_static(backward_propagation_cuda * self, run_kernels_params * params)
		{
			self->run_kernels(*params);
		}

		backward_propagation_cuda::run_kernels_params::run_kernels_params(
			std::map<std::string, nnforge_array<cuda_linear_buffer_device::ptr, 2> >& dedicated_buffers,
			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >& net_data,
			std::map<std::string, std::vector<cuda_linear_buffer_device::const_ptr> >& net_data_custom,
			std::map<std::string, std::vector<cuda_linear_buffer_device::const_ptr> >& persistent_working_data,
			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >& gradient,
			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >& previous_upd,
			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >& previous_upd2,
			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >& update_accum_buffers,
			const std::map<std::string, std::vector<float> >& learning_rates,
			unsigned int batch_size,
			float weight_decay,
			training_momentum momentum,
			unsigned int max_chunk_size,
			unsigned int base_iteration_count)
			: dedicated_buffers(dedicated_buffers)
			, net_data(net_data)
			, net_data_custom(net_data_custom)
			, persistent_working_data(persistent_working_data)
			, gradient(gradient)
			, previous_upd(previous_upd)
			, previous_upd2(previous_upd2)
			, update_accum_buffers(update_accum_buffers)
			, learning_rates(learning_rates)
			, batch_size(batch_size)
			, weight_decay(weight_decay)
			, momentum(momentum)
			, max_chunk_size(max_chunk_size)
			, base_iteration_count(base_iteration_count)
			, gradient_applied_count(0)
		{
		}

		void backward_propagation_cuda::setup_network_cuda()
		{
			copy_data_stream = cuda_stream::ptr(new cuda_stream());
		}

		void backward_propagation_cuda::setup_streams_and_events()
		{
			command_streams.clear();
			action_to_stream_set_map.clear();
			action_output_data_ready_events.clear();
			action_previous_events.clear();
			output_data_ready_additional_events.clear();
			start_stop_profiling_events.clear();

			std::vector<std::vector<layer_name_with_action> > layer_stream_set = optimized_action_schema->get_action_stream_set();

			if (cuda_config->is_single_command_stream())
			{
				std::vector<std::vector<layer_name_with_action> > layer_stream_set_orig = layer_stream_set;
				layer_stream_set.clear();
				layer_stream_set.push_back(std::vector<layer_name_with_action>());
				std::vector<layer_name_with_action>& new_layer_list = layer_stream_set.front();
				for(std::vector<std::vector<layer_name_with_action> >::const_iterator it = layer_stream_set_orig.begin(); it != layer_stream_set_orig.end(); ++it)
				{
					const std::vector<layer_name_with_action>& ll = *it;
					for(std::vector<layer_name_with_action>::const_iterator it2 = ll.begin(); it2 != ll.end(); ++it2)
						new_layer_list.push_back(*it2);
				}
			}

			command_streams.resize(layer_stream_set.size());
			for(unsigned int stream_set_id = 0; stream_set_id < static_cast<unsigned int>(layer_stream_set.size()); ++stream_set_id)
			{
				command_streams[stream_set_id] = cuda_stream::ptr(new cuda_stream());
				for(std::vector<layer_name_with_action>::const_iterator it = layer_stream_set[stream_set_id].begin(); it != layer_stream_set[stream_set_id].end(); ++it)
					action_to_stream_set_map.insert(std::make_pair(*it, stream_set_id));
			}
			if (debug->is_debug())
			{
				debug->output_message((boost::format("backward prop cuda streams: %1%") % layer_stream_set.size()).str().c_str());
				boost::filesystem::ofstream out(debug->get_path_to_unique_file("backward_prop_cuda_streams", "gv"), std::ios_base::out | std::ios_base::trunc);
				optimized_action_schema->write_gv(out, action_to_stream_set_map);
			}

			for(std::vector<layer_name_with_action>::const_reverse_iterator it = actions_in_execution_order.rbegin(); it != actions_in_execution_order.rend(); ++it)
			{
				unsigned int current_stream_set_id = action_to_stream_set_map.find(*it)->second;

				std::vector<cuda_event::ptr> previous_events;
				std::vector<layer_name_with_action> previous_actions = optimized_action_schema->get_dependencies(*it);
				for(std::vector<layer_name_with_action>::const_iterator it2 = previous_actions.begin(); it2 != previous_actions.end(); ++it2)
				{
					const layer_name_with_action& previous_layer_action = *it2;

					unsigned int previous_stream_set_id = action_to_stream_set_map.find(previous_layer_action)->second;
					if (previous_stream_set_id == current_stream_set_id)
						continue;

					cuda_event::ptr previous_event;
					std::map<layer_name_with_action, cuda_event::ptr>::const_iterator it3 = action_output_data_ready_events.find(previous_layer_action);
					if (it3 != action_output_data_ready_events.end())
						previous_event = it3->second;
					else
						previous_event = action_output_data_ready_events.insert(std::make_pair(previous_layer_action, cuda_event::ptr(new cuda_event()))).first->second;
					previous_events.push_back(previous_event);
				}

				if (!previous_events.empty())
					action_previous_events.insert(std::make_pair(*it, previous_events));
			}

			std::vector<layer_name_with_action> target_actions;
			{
				for(std::vector<std::string>::const_iterator it = output_layer_names.begin(); it != output_layer_names.end(); ++it)
					target_actions.push_back(layer_name_with_action(*it, layer_action::forward));
				for(std::vector<layer_name_with_action>::const_iterator it = actions_in_execution_order.begin(); it != actions_in_execution_order.end(); ++it)
					if (it->get_action().get_action_type() == layer_action::update_weights)
						target_actions.push_back(*it);
			}

			bool output_data_ready_stream_set_id_defined = false;
			for(std::vector<layer_name_with_action>::const_iterator it = target_actions.begin(); it != target_actions.end(); ++it)
			{
				if (!output_data_ready_stream_set_id_defined)
				{
					output_data_ready_stream_set_id = action_to_stream_set_map[*it];
					output_data_ready_stream_set_id_defined = true;
					continue;
				}
				else
				{
					if (action_to_stream_set_map[*it] == output_data_ready_stream_set_id)
						continue;
				}

				cuda_event::ptr previous_event;
				std::map<layer_name_with_action, cuda_event::ptr>::const_iterator it3 = action_output_data_ready_events.find(*it);
				if (it3 != action_output_data_ready_events.end())
					previous_event = it3->second;
				else
					previous_event = action_output_data_ready_events.insert(std::make_pair(*it, cuda_event::ptr(new cuda_event()))).first->second;
				output_data_ready_additional_events.push_back(previous_event);
			}

			if (profile->is_profile())
			{
				for(std::vector<layer_name_with_action>::const_iterator it = actions_in_execution_order.begin(); it != actions_in_execution_order.end(); ++it)
					start_stop_profiling_events.insert(std::make_pair(*it, std::make_pair(cuda_event::ptr(new cuda_event(true)), cuda_event::ptr(new cuda_event(true)))));
			}
		}

		void backward_propagation_cuda::setup_optimized_action_schema()
		{
			{
				network_action_schema::ptr optimized_action_schema_tmp = network_action_schema::ptr(new network_action_schema(*action_schema));
				float saturation_flops = cuda_config->get_flops() * cuda_config->get_device_saturation_time() / static_cast<float>(cuda_config->optimize_action_graph_assumed_chunk_size);
				optimized_action_schema_tmp->add_dependencies_for_distant_otherwise_inependent_actions(
					layer_config_map,
					cumulative_tiling_factor_map,
					saturation_flops);
				optimized_action_schema = optimized_action_schema_tmp;
			}

			if (debug->is_debug())
			{
				std::vector<layer_name_with_action> actions = optimized_action_schema->get_actions();
				std::map<layer_name_with_action, unsigned int> layer_name_with_action_color_map;
				for(std::vector<layer_name_with_action>::const_iterator it = actions.begin(); it != actions.end(); ++it)
				{
					unsigned int color_id;
					switch (it->get_action().get_action_type())
					{
					case layer_action::forward:
						color_id = 0;
						break;
					case layer_action::backward_data:
						color_id = 1;
						break;
					case layer_action::backward_weights:
						color_id = 2;
						break;
					case layer_action::backward_data_and_weights:
						color_id = 3;
						break;
					case layer_action::update_weights:
						color_id = 4;
						break;
					default:
						color_id = 5;
						break;
					}
					layer_name_with_action_color_map.insert(std::make_pair(*it, color_id));
				}

				boost::filesystem::ofstream out(debug->get_path_to_unique_file("backward_prop_optimized_action_schema", "gv"), std::ios_base::out | std::ios_base::trunc);
				optimized_action_schema->write_gv(out, layer_name_with_action_color_map);
			}

			actions_in_execution_order = optimized_action_schema->get_actions_in_execution_order();
		}

		void backward_propagation_cuda::layer_config_map_modified()
		{
			cuda_config->set_device();

			setup_optimized_action_schema();

			setup_streams_and_events();

			updaters.clear();

			setup_io_host_buffer_sizes();

			setup_dedicated_buffer_sizes();

			std::map<std::string, std::set<layer_action> > layer_name_to_action_set_map;
			for(std::vector<layer_name_with_action>::const_iterator it = actions_in_execution_order.begin(); it != actions_in_execution_order.end(); ++it)
				layer_name_to_action_set_map.insert(std::make_pair(it->get_name(), std::set<layer_action>())).first->second.insert(it->get_action());
			for(std::map<std::string, layer_updater_schema::const_ptr>::const_iterator it = updater_schemas.begin(); it != updater_schemas.end(); ++it)
			{
				const std::string& layer_name = it->first;
				layer_configuration_specific output_layer_configuration_specific = layer_config_map[layer_name];
				layer::const_ptr l = schema->get_layer(layer_name);
				std::vector<layer_configuration_specific> input_layer_configuration_specific_list;
				for(std::vector<std::string>::const_iterator it2 = l->input_layer_instance_names.begin(); it2 != l->input_layer_instance_names.end(); ++it2)
					input_layer_configuration_specific_list.push_back(layer_config_map[*it2]);

				updaters.insert(
					std::make_pair(
						l->instance_name,
						it->second->create_updater(
							input_layer_configuration_specific_list,
							output_layer_configuration_specific,
							layer_name_to_action_set_map[layer_name])));
			}

			setup_per_entry_buffer_sizes();

			setup_fixed_buffer_sizes();

			update_buffer_config();
		}

		void backward_propagation_cuda::setup_io_host_buffer_sizes()
		{
			input_per_entry_host_data_name_to_size_map.clear();
			output_per_entry_host_data_name_to_size_map.clear();

			for(std::set<std::string>::const_iterator it = data_layer_names.begin(); it != data_layer_names.end(); ++it)
				input_per_entry_host_data_name_to_size_map.insert(std::make_pair(*it, layer_config_map.find(*it)->second.get_neuron_count() * cumulative_tiling_factor_map[*it] * sizeof(float)));
			for(std::vector<std::string>::const_iterator it = output_layer_names.begin(); it != output_layer_names.end(); ++it)
				output_per_entry_host_data_name_to_size_map.insert(std::make_pair(*it, layer_config_map.find(*it)->second.get_neuron_count() * cumulative_tiling_factor_map[*it] * sizeof(float)));
		}

		void backward_propagation_cuda::setup_dedicated_buffer_sizes()
		{
			dedicated_per_entry_data_name_to_size_map.clear();

			std::set<std::string> separate_buffers_layer_names(output_layer_names.begin(), output_layer_names.end());
			separate_buffers_layer_names.insert(data_layer_names.begin(), data_layer_names.end());
			for(std::set<std::string>::const_iterator it = separate_buffers_layer_names.begin(); it != separate_buffers_layer_names.end(); ++it)
				dedicated_per_entry_data_name_to_size_map.insert(std::make_pair(*it, layer_config_map.find(*it)->second.get_neuron_count() * cumulative_tiling_factor_map[*it] * sizeof(float)));
		}

		void backward_propagation_cuda::setup_fixed_buffer_sizes()
		{
			size_t max_fixed_working_buffers_size = cuda_config->get_max_fixed_working_buffers_size();

			std::vector<std::vector<std::pair<layer_name_with_action, buffer_lifetime> > > fixed_buffer_set_list;

			{
				std::map<layer_name_with_action, std::vector<std::pair<buffer_lifetime, float> > > buffers;
				std::map<layer_name_with_action, std::map<layer_name_with_action, std::vector<std::pair<buffer_lifetime, bool> > > > dependencies;
				for(std::vector<layer_name_with_action>::const_iterator it = actions_in_execution_order.begin(); it != actions_in_execution_order.end(); ++it)
				{
					std::string layer_name = it->get_name();
					layer_updater_cuda::ptr updater = updaters[layer_name];
					std::vector<std::pair<buffer_lifetime, float> > current_buffers;
					{
						if (it->get_action().get_action_type() == layer_action::forward)
						{
							size_t temporary_fixed_buffer_size = updater->get_temporary_fixed_buffer_size();
							if (temporary_fixed_buffer_size > 0)
								current_buffers.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::temporary_buffer), static_cast<float>(temporary_fixed_buffer_size)));
						}

						{
							std::pair<size_t, bool> temporary_working_fixed_buffer_size_and_flag = updaters[it->get_name()]->get_temporary_working_fixed_buffer_size(it->get_action());
							size_t temporary_working_fixed_buffer_size = temporary_working_fixed_buffer_size_and_flag.first;
							if (temporary_working_fixed_buffer_size_and_flag.second)
								temporary_working_fixed_buffer_size = std::max(temporary_working_fixed_buffer_size, max_fixed_working_buffers_size);
							if (temporary_working_fixed_buffer_size > 0)
								buffers.insert(std::make_pair(*it, std::vector<std::pair<buffer_lifetime, float> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::working_buffer), static_cast<float>(temporary_working_fixed_buffer_size)));
						}
					}

					if (!current_buffers.empty())
						buffers.insert(std::make_pair(*it, current_buffers));

					std::map<layer_name_with_action, std::vector<std::pair<buffer_lifetime, bool> > > current_dependencies;
					{
						layer::const_ptr l = schema->get_layer(layer_name);
						switch (it->get_action().get_action_type())
						{
						case layer_action::backward_weights:
							{
								if (updater->is_backward_weights_dependent_on_temporary_fixed_buffer())
									current_dependencies.insert(std::make_pair(layer_name_with_action(it->get_name(), layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::temporary_buffer), false));
							}
							break;
						case layer_action::backward_data:
							{
								unsigned int action_input_index = it->get_action().get_backprop_index();
								if (updater->is_backward_data_dependent_on_temporary_fixed_buffer(action_input_index))
									current_dependencies.insert(std::make_pair(layer_name_with_action(it->get_name(), layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::temporary_buffer), false));
							}
							break;
						case layer_action::backward_data_and_weights:
							{
								if (updater->is_backward_data_and_weights_dependent_on_temporary_fixed_buffer())
									current_dependencies.insert(std::make_pair(layer_name_with_action(it->get_name(), layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::temporary_buffer), false));
							}
							break;
						}
					}

					if (!current_dependencies.empty())
						dependencies.insert(std::make_pair(*it, current_dependencies));
				}

				fixed_buffer_set_list = optimized_action_schema->get_buffer_set(
					buffers,
					dependencies,
					std::vector<std::vector<std::pair<layer_name_with_action, buffer_lifetime> > >());

				if (cuda_config->is_dont_share_buffers())
				{
					std::vector<std::vector<std::pair<layer_name_with_action, buffer_lifetime> > > temporary_working_fixed_buffer_set_list_orig = fixed_buffer_set_list;
					fixed_buffer_set_list.clear();
					for(unsigned int set_id = 0; set_id < temporary_working_fixed_buffer_set_list_orig.size(); ++set_id)
					{
						const std::vector<std::pair<layer_name_with_action, buffer_lifetime> >& action_list = temporary_working_fixed_buffer_set_list_orig[set_id];
						for(std::vector<std::pair<layer_name_with_action, buffer_lifetime> >::const_iterator it = action_list.begin(); it != action_list.end(); ++it)
							fixed_buffer_set_list.push_back(std::vector<std::pair<layer_name_with_action, buffer_lifetime> >(1, *it));
					}
				}
			}

			fixed_set_size_list.clear();
			temporary_working_fixed_data_action_to_set_map.clear();
			temporary_fixed_data_action_to_set_map.clear();

			std::set<unsigned int> set_ids_with_hungry_working_buffers;
			for(unsigned int set_id = 0; set_id < fixed_buffer_set_list.size(); ++set_id)
			{
				const std::vector<std::pair<layer_name_with_action, buffer_lifetime> >& action_list = fixed_buffer_set_list[set_id];
				for(std::vector<std::pair<layer_name_with_action, buffer_lifetime> >::const_iterator it = action_list.begin(); it != action_list.end(); ++it)
				{
					std::string layer_name = it->first.get_name();
					if ((it->second.get_buffer_lifetime_type() == buffer_lifetime::working_buffer) && updaters[layer_name]->get_temporary_working_fixed_buffer_size(it->first.get_action()).second)
						set_ids_with_hungry_working_buffers.insert(set_id);
				}
			}
			if (set_ids_with_hungry_working_buffers.size() > 1)
				max_fixed_working_buffers_size /= set_ids_with_hungry_working_buffers.size();

			for(unsigned int set_id = 0; set_id < fixed_buffer_set_list.size(); ++set_id)
			{
				const std::vector<std::pair<layer_name_with_action, buffer_lifetime> >& action_list = fixed_buffer_set_list[set_id];
				size_t max_buffer_size = (set_ids_with_hungry_working_buffers.find(set_id) != set_ids_with_hungry_working_buffers.end()) ? max_fixed_working_buffers_size : 1;
				for(std::vector<std::pair<layer_name_with_action, buffer_lifetime> >::const_iterator it = action_list.begin(); it != action_list.end(); ++it)
				{
					std::string layer_name = it->first.get_name();
					size_t buffer_size;
					switch(it->second.get_buffer_lifetime_type())
					{
					case buffer_lifetime::working_buffer:
						temporary_working_fixed_data_action_to_set_map.insert(std::make_pair(it->first, set_id));
						buffer_size = updaters[layer_name]->get_temporary_working_fixed_buffer_size(it->first.get_action()).first;
						break;
					case buffer_lifetime::temporary_buffer:
						temporary_fixed_data_action_to_set_map.insert(std::make_pair(it->first, set_id));
						buffer_size = updaters[layer_name]->get_temporary_fixed_buffer_size();
						break;
					default:
						throw neural_network_exception((boost::format("Unexpected buffer lifetime %1% encountered for layer %2% action %3%") % it->second.str() % it->first.get_name() % it->first.get_action().str()).str());
					}
					max_buffer_size = std::max(max_buffer_size, buffer_size);
				}
				fixed_set_size_list.push_back(max_buffer_size);
			}

			if (debug->is_debug())
			{
				std::stringstream debug_str;
				debug_str << "backward prop cuda per fixed buffers: " << fixed_set_size_list.size();
				size_t total_buffer_size = 0;
				for(std::vector<size_t>::const_iterator it = fixed_set_size_list.begin(); it != fixed_set_size_list.end(); ++it)
						total_buffer_size += *it;
				debug_str << ", total size " << ((total_buffer_size + (1024 * 1024) - 1) / (1024 * 1024)) << " MB";
				debug->output_message(debug_str.str().c_str());
				for(unsigned int set_id = 0; set_id < static_cast<unsigned int>(fixed_set_size_list.size()); ++set_id)
				{
					std::stringstream debug_str;
					debug_str << " - " << ((fixed_set_size_list[set_id] + (1024 * 1024) - 1) / (1024 * 1024)) << " MB: ";
					const std::vector<std::pair<layer_name_with_action, buffer_lifetime> >& action_list= fixed_buffer_set_list[set_id];
					for(std::vector<std::pair<layer_name_with_action, buffer_lifetime> >::const_iterator it = action_list.begin(); it != action_list.end(); ++it)
					{
						if (it != action_list.begin())
							debug_str << ", ";
						debug_str << it->first.get_name() << " " << it->first.get_action().str() << " " << it->second.str();
					}
					debug->output_message(debug_str.str().c_str());
				}
				boost::filesystem::ofstream out(debug->get_path_to_unique_file("backward_prop_cuda_fixed_buffers", "gv"), std::ios_base::out | std::ios_base::trunc);
				optimized_action_schema->write_gv(out, std::map<layer_name_with_action, unsigned int>() ,temporary_fixed_data_action_to_set_map, temporary_working_fixed_data_action_to_set_map);
			}
		}

		void backward_propagation_cuda::setup_per_entry_buffer_sizes()
		{
			std::vector<std::vector<std::pair<layer_name_with_action, buffer_lifetime> > > layer_buffer_set_list;
			{
				std::map<layer_name_with_action, std::vector<std::pair<buffer_lifetime, float> > > buffers;
				std::map<layer_name_with_action, std::map<layer_name_with_action, std::vector<std::pair<buffer_lifetime, bool> > > > dependencies;
				std::set<std::string> dedicated_output_buffers(output_layer_names.begin(), output_layer_names.end());
				for(std::vector<layer_name_with_action>::const_iterator it = actions_in_execution_order.begin(); it != actions_in_execution_order.end(); ++it)
				{
					std::string layer_name = it->get_name();
					layer_updater_cuda::ptr updater = updaters[layer_name];
					std::vector<std::pair<buffer_lifetime, float> > current_buffers;
					{
						switch (it->get_action().get_action_type())
						{
						case layer_action::forward:
							{
								size_t buffer_size_per_entry = layer_config_map.find(layer_name)->second.get_neuron_count() * cumulative_tiling_factor_map[layer_name] * sizeof(float);
								if (dedicated_output_buffers.find(layer_name) == dedicated_output_buffers.end())
									current_buffers.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), static_cast<float>(buffer_size_per_entry)));
							}
							{
								size_t temporary_per_entry_buffer_size = updater->get_temporary_per_entry_buffer_size() * cumulative_tiling_factor_map[layer_name];
								if (temporary_per_entry_buffer_size > 0)
									current_buffers.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::temporary_buffer), static_cast<float>(temporary_per_entry_buffer_size)));
							}
							break;
						case layer_action::backward_data:
							{
								const std::string& previous_layer_name = schema->get_layer(layer_name)->input_layer_instance_names[it->get_action().get_backprop_index()];
								size_t buffer_size_per_entry = layer_config_map.find(previous_layer_name)->second.get_neuron_count() * cumulative_tiling_factor_map[previous_layer_name] * sizeof(float);
								current_buffers.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), static_cast<float>(buffer_size_per_entry)));
							}
							break;
						case layer_action::backward_data_and_weights:
							{
								if (schema->get_layer(layer_name)->input_layer_instance_names.size() != 1)
									throw neural_network_exception((boost::format("setup_layer_buffer_sizes cannot handle multiple output buffers for action %1% for layer %2%") % it->get_action().str() % it->get_name()).str());
								const std::string& previous_layer_name = schema->get_layer(layer_name)->input_layer_instance_names[0];
								size_t buffer_size_per_entry = layer_config_map.find(previous_layer_name)->second.get_neuron_count() * cumulative_tiling_factor_map[previous_layer_name] * sizeof(float);
								current_buffers.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), static_cast<float>(buffer_size_per_entry)));
							}
							break;
						}

						{
							size_t temporary_working_per_entry_buffer_size = updater->get_temporary_working_per_entry_buffer_size(it->get_action()) * cumulative_tiling_factor_map[layer_name];
							if (temporary_working_per_entry_buffer_size > 0)
								current_buffers.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::working_buffer), static_cast<float>(temporary_working_per_entry_buffer_size)));
						}
					}

					if (!current_buffers.empty())
						buffers.insert(std::make_pair(*it, current_buffers));

					int input_index_layer_can_write = updaters[it->get_name()]->get_input_index_layer_can_write(it->get_action());
					std::map<layer_name_with_action, std::vector<std::pair<buffer_lifetime, bool> > > current_dependencies;
					{
						layer::const_ptr l = schema->get_layer(layer_name);
						switch (it->get_action().get_action_type())
						{
						case layer_action::forward:
							{
								int input_index = 0;
								for(std::vector<std::string>::const_iterator it2 = l->input_layer_instance_names.begin(); it2 != l->input_layer_instance_names.end(); ++it2, ++input_index)
								{
									const std::string& previous_layer_name = *it2;
									if (data_layer_names.find(previous_layer_name) == data_layer_names.end())
										current_dependencies.insert(std::make_pair(layer_name_with_action(previous_layer_name, layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(
										std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), (input_index_layer_can_write == input_index)));
								}
							}
							break;
						case layer_action::backward_weights:
							{
								unsigned int data_input_index = 0;
								for(std::vector<std::string>::const_iterator it2 = l->input_layer_instance_names.begin(); it2 != l->input_layer_instance_names.end(); ++it2, ++data_input_index)
								{
									const std::string& previous_layer_name = *it2;
									if ((data_layer_names.find(previous_layer_name) == data_layer_names.end()) && updater->is_backward_weights_dependent_on_input_buffer(data_input_index))
										current_dependencies.insert(std::make_pair(layer_name_with_action(previous_layer_name, layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), false));
								}
								std::map<std::string, std::vector<layer_name_with_action> >::const_iterator input_to_all_output_it = input_to_all_output_map.find(l->instance_name);
								if (input_to_all_output_it != input_to_all_output_map.end())
									for(std::vector<layer_name_with_action>::const_iterator src_it = input_to_all_output_it->second.begin(); src_it != input_to_all_output_it->second.end(); ++src_it)
										current_dependencies.insert(std::make_pair(*src_it, std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), false));
								if (updater->is_backward_weights_dependent_on_temporary_per_entry_buffer())
									current_dependencies.insert(std::make_pair(layer_name_with_action(it->get_name(), layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::temporary_buffer), false));
							}
							break;
						case layer_action::backward_data:
							{
								unsigned int action_input_index = it->get_action().get_backprop_index();
								unsigned int data_input_index = 0;
								for(std::vector<std::string>::const_iterator it2 = l->input_layer_instance_names.begin(); it2 != l->input_layer_instance_names.end(); ++it2, ++data_input_index)
								{
									const std::string& previous_layer_name = *it2;
									if ((data_layer_names.find(previous_layer_name) == data_layer_names.end()) && updater->is_backward_data_dependent_on_input_buffer(action_input_index, data_input_index))
										current_dependencies.insert(std::make_pair(layer_name_with_action(previous_layer_name, layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), false));
								}
								if (updater->is_backward_data_dependent_on_output_buffer(action_input_index))
									current_dependencies.insert(std::make_pair(layer_name_with_action(it->get_name(), layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), false));
								std::map<std::string, std::vector<layer_name_with_action> >::const_iterator input_to_all_output_it = input_to_all_output_map.find(l->instance_name);
								if (input_to_all_output_it != input_to_all_output_map.end())
									for(std::vector<layer_name_with_action>::const_iterator src_it = input_to_all_output_it->second.begin(); src_it != input_to_all_output_it->second.end(); ++src_it)
										current_dependencies.insert(std::make_pair(*src_it, std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), (input_index_layer_can_write == 0)));
								if (updater->is_backward_data_dependent_on_temporary_per_entry_buffer(action_input_index))
									current_dependencies.insert(std::make_pair(layer_name_with_action(it->get_name(), layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::temporary_buffer), false));
							}
							break;
						case layer_action::backward_data_and_weights:
							{
								unsigned int data_input_index = 0;
								for(std::vector<std::string>::const_iterator it2 = l->input_layer_instance_names.begin(); it2 != l->input_layer_instance_names.end(); ++it2, ++data_input_index)
								{
									const std::string& previous_layer_name = *it2;
									if ((data_layer_names.find(previous_layer_name) == data_layer_names.end()) && updater->is_backward_data_and_weights_dependent_on_input_buffer(data_input_index))
										current_dependencies.insert(std::make_pair(layer_name_with_action(previous_layer_name, layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), false));
								}
								if (updater->is_backward_data_and_weights_dependent_on_output_buffer())
									current_dependencies.insert(std::make_pair(layer_name_with_action(it->get_name(), layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), false));
								std::map<std::string, std::vector<layer_name_with_action> >::const_iterator input_to_all_output_it = input_to_all_output_map.find(l->instance_name);
								if (input_to_all_output_it != input_to_all_output_map.end())
									for(std::vector<layer_name_with_action>::const_iterator src_it = input_to_all_output_it->second.begin(); src_it != input_to_all_output_it->second.end(); ++src_it)
										current_dependencies.insert(std::make_pair(*src_it, std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::action_output_buffer), (input_index_layer_can_write == 0)));
								if (updater->is_backward_data_and_weights_dependent_on_temporary_per_entry_buffer())
									current_dependencies.insert(std::make_pair(layer_name_with_action(it->get_name(), layer_action(layer_action::forward)), std::vector<std::pair<buffer_lifetime, bool> >())).first->second.push_back(std::make_pair(buffer_lifetime(buffer_lifetime::temporary_buffer), false));
							}
							break;
						}
					}

					if (!current_dependencies.empty())
						dependencies.insert(std::make_pair(*it, current_dependencies));
				}

				std::vector<std::vector<std::pair<layer_name_with_action, buffer_lifetime> > > should_be_placed_into_the_same_buffers;
				for(std::vector<std::vector<layer_name_with_action> >::const_iterator it = same_output_action_sets.begin(); it != same_output_action_sets.end(); ++it)
				{
					const std::vector<layer_name_with_action>& src_tt = *it;
					should_be_placed_into_the_same_buffers.push_back(std::vector<std::pair<layer_name_with_action, buffer_lifetime> >());
					std::vector<std::pair<layer_name_with_action, buffer_lifetime> >& tt = should_be_placed_into_the_same_buffers.back();
					for(std::vector<layer_name_with_action>::const_iterator it2 = src_tt.begin(); it2 != src_tt.end(); ++it2)
						tt.push_back(std::make_pair(*it2, buffer_lifetime(buffer_lifetime::action_output_buffer)));
				}

				layer_buffer_set_list = optimized_action_schema->get_buffer_set(
					buffers,
					dependencies,
					should_be_placed_into_the_same_buffers);

				if (cuda_config->is_dont_share_buffers())
				{
					std::vector<std::vector<std::pair<layer_name_with_action, buffer_lifetime> > > layer_buffer_set_list_orig = layer_buffer_set_list;
					layer_buffer_set_list = should_be_placed_into_the_same_buffers;

					std::map<layer_name_with_action, std::set<buffer_lifetime> > same_buffers;
					for(std::vector<std::vector<std::pair<layer_name_with_action, buffer_lifetime> > >::const_iterator it = should_be_placed_into_the_same_buffers.begin(); it != should_be_placed_into_the_same_buffers.end(); ++it)
					{
						const std::vector<std::pair<layer_name_with_action, buffer_lifetime> >& list = *it;
						for(std::vector<std::pair<layer_name_with_action, buffer_lifetime> >::const_iterator it2 = list.begin(); it2 != list.end(); ++it2)
							same_buffers.insert(std::make_pair(it2->first, std::set<buffer_lifetime>())).first->second.insert(it2->second);
					}

					for(unsigned int set_id = 0; set_id < layer_buffer_set_list_orig.size(); ++set_id)
					{
						const std::vector<std::pair<layer_name_with_action, buffer_lifetime> >& action_list = layer_buffer_set_list_orig[set_id];
						for(std::vector<std::pair<layer_name_with_action, buffer_lifetime> >::const_iterator it = action_list.begin(); it != action_list.end(); ++it)
						{
							bool is_buffer_processed = false;
							std::map<layer_name_with_action, std::set<buffer_lifetime> >::const_iterator tt1 = same_buffers.find(it->first);
							if (tt1 != same_buffers.end())
							{
								std::set<buffer_lifetime>::const_iterator tt2 = tt1->second.find(it->second);
								if (tt2 != tt1->second.end())
									is_buffer_processed = true;
							}
							if (!is_buffer_processed)
								layer_buffer_set_list.push_back(std::vector<std::pair<layer_name_with_action, buffer_lifetime> >(1, *it));
						}
					}
				}
			}

			layer_buffer_set_per_entry_size_list.clear();
			layer_buffer_action_to_set_map.clear();
			temporary_working_per_entry_data_action_to_set_map.clear();
			temporary_per_entry_data_action_to_set_map.clear();
			for(unsigned int set_id = 0; set_id < layer_buffer_set_list.size(); ++set_id)
			{
				const std::vector<std::pair<layer_name_with_action, buffer_lifetime> >& action_list = layer_buffer_set_list[set_id];
				size_t max_buffer_size_per_entry = 0;
				for(std::vector<std::pair<layer_name_with_action, buffer_lifetime> >::const_iterator it = action_list.begin(); it != action_list.end(); ++it)
				{
					std::string layer_name = it->first.get_name();
					size_t buffer_size_per_entry;
					switch(it->second.get_buffer_lifetime_type())
					{
					case buffer_lifetime::action_output_buffer:
						layer_buffer_action_to_set_map.insert(std::make_pair(it->first, set_id));
						switch (it->first.get_action().get_action_type())
						{
						case layer_action::forward:
							buffer_size_per_entry = layer_config_map.find(layer_name)->second.get_neuron_count() * cumulative_tiling_factor_map[layer_name] * sizeof(float);
							break;
						case layer_action::backward_data:
							{
								const std::string& previous_layer_name = schema->get_layer(layer_name)->input_layer_instance_names[it->first.get_action().get_backprop_index()];
								buffer_size_per_entry = layer_config_map.find(previous_layer_name)->second.get_neuron_count() * cumulative_tiling_factor_map[previous_layer_name] * sizeof(float);
							}
							break;
						case layer_action::backward_data_and_weights:
							{
								const std::string& previous_layer_name = schema->get_layer(layer_name)->input_layer_instance_names[0];
								buffer_size_per_entry = layer_config_map.find(previous_layer_name)->second.get_neuron_count() * cumulative_tiling_factor_map[previous_layer_name] * sizeof(float);
							}
							break;
						default:
							throw neural_network_exception((boost::format("Unexpected buffer lifetime %1% encountered for layer %2% action %3%") % it->second.str() % it->first.get_name() % it->first.get_action().str()).str());
						}
						break;
					case buffer_lifetime::working_buffer:
						temporary_working_per_entry_data_action_to_set_map.insert(std::make_pair(it->first, set_id));
						buffer_size_per_entry = updaters[layer_name]->get_temporary_working_per_entry_buffer_size(it->first.get_action()) * cumulative_tiling_factor_map[layer_name];
						break;
					case buffer_lifetime::temporary_buffer:
						temporary_per_entry_data_action_to_set_map.insert(std::make_pair(it->first, set_id));
						buffer_size_per_entry = updaters[layer_name]->get_temporary_per_entry_buffer_size() * cumulative_tiling_factor_map[layer_name];
						break;
					default:
						throw neural_network_exception((boost::format("Unexpected buffer lifetime %1% encountered for layer %2% action %3%") % it->second.str() % it->first.get_name() % it->first.get_action().str()).str());
					}
					max_buffer_size_per_entry = std::max(max_buffer_size_per_entry, buffer_size_per_entry);
				}
				layer_buffer_set_per_entry_size_list.push_back(max_buffer_size_per_entry);
			}

			if (debug->is_debug())
			{
				std::stringstream debug_str;
				debug_str << "backward prop cuda per entry buffers: " << layer_buffer_set_per_entry_size_list.size();
				size_t total_buffer_size = 0;
				for(std::vector<size_t>::const_iterator it = layer_buffer_set_per_entry_size_list.begin(); it != layer_buffer_set_per_entry_size_list.end(); ++it)
						total_buffer_size += *it;
				debug_str << ", total size " << ((total_buffer_size + 1024 - 1) / 1024) << " KB";
				debug->output_message(debug_str.str().c_str());
				for(unsigned int set_id = 0; set_id < static_cast<unsigned int>(layer_buffer_set_per_entry_size_list.size()); ++set_id)
				{
					std::stringstream debug_str;
					debug_str << " - " << ((layer_buffer_set_per_entry_size_list[set_id] + 1024 - 1) / 1024) << " KB: ";
					const std::vector<std::pair<layer_name_with_action, buffer_lifetime> >& action_list = layer_buffer_set_list[set_id];
					for(std::vector<std::pair<layer_name_with_action, buffer_lifetime> >::const_iterator it = action_list.begin(); it != action_list.end(); ++it)
					{
						if (it != action_list.begin())
							debug_str << ", ";
						debug_str << it->first.get_name() << " " << it->first.get_action().str();
						if (it->second.get_buffer_lifetime_type() != buffer_lifetime::action_output_buffer)
							debug_str << " " << it->second.str();
					}
					debug->output_message(debug_str.str().c_str());
				}
				boost::filesystem::ofstream out(debug->get_path_to_unique_file("backward_prop_cuda_per_entry_buffers", "gv"), std::ios_base::out | std::ios_base::trunc);
				optimized_action_schema->write_gv(out, layer_buffer_action_to_set_map, temporary_per_entry_data_action_to_set_map, temporary_working_per_entry_data_action_to_set_map);
			}
		}

		void backward_propagation_cuda::update_buffer_config()
		{
			buffer_cuda_size_configuration buffer_configuration;

			for(std::map<std::string, std::vector<cuda_linear_buffer_device::const_ptr> >::const_iterator it = schema_data.begin(); it != schema_data.end(); ++it)
				for(std::vector<cuda_linear_buffer_device::const_ptr>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2)
					buffer_configuration.add_constant_buffer((*it2)->get_size());

			for(std::vector<size_t>::const_iterator it = layer_buffer_set_per_entry_size_list.begin(); it != layer_buffer_set_per_entry_size_list.end(); ++it)
				buffer_configuration.add_per_entry_buffer(*it);
			for(std::map<std::string, size_t>::const_iterator it = dedicated_per_entry_data_name_to_size_map.begin(); it != dedicated_per_entry_data_name_to_size_map.end(); ++it)
			{
				// 2 buffers for concurrent input and output data transfer
				buffer_configuration.add_per_entry_buffer(it->second);
				buffer_configuration.add_per_entry_buffer(it->second);
			}
			for(std::vector<size_t>::const_iterator it = fixed_set_size_list.begin(); it != fixed_set_size_list.end(); ++it)
				buffer_configuration.add_constant_buffer(*it);

			for(std::map<std::string, layer_updater_cuda::ptr>::const_iterator it = updaters.begin(); it != updaters.end(); ++it)
			{
				std::vector<unsigned int> tex_per_entry = it->second->get_linear_addressing_through_texture_per_entry();
				unsigned int cumulative_tiling_factor = cumulative_tiling_factor_map[it->first];
				for(std::vector<unsigned int>::const_iterator it2 = tex_per_entry.begin(); it2 != tex_per_entry.end(); ++it2)
					buffer_configuration.add_per_entry_linear_addressing_through_texture(*it2 * cumulative_tiling_factor);
			}

			buffer_config_without_data_and_momentum = buffer_configuration;
		}

		std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> > backward_propagation_cuda::get_data(const layer_data_list& host_data) const
		{
			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> > res;

			for(std::map<std::string, layer_updater_cuda::ptr>::const_iterator it = updaters.begin(); it != updaters.end(); ++it)
			{
				layer_data::const_ptr dt = host_data.find(it->first);
				if (dt)
					res.insert(std::make_pair(it->first, it->second->get_data(dt)));
			}

			return res;
		}

		std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> > backward_propagation_cuda::get_zero_gradient(const std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >& net_data) const
		{
			std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> > res;
			
			for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = net_data.begin(); it != net_data.end(); ++it)
			{
				std::vector<cuda_linear_buffer_device::ptr>& dst = res.insert(std::make_pair(it->first, std::vector<cuda_linear_buffer_device::ptr>())).first->second;
				for(std::vector<cuda_linear_buffer_device::ptr>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2)
				{
					size_t buf_size = (*it2)->get_size();
					cuda_linear_buffer_device::ptr buf = cuda_linear_buffer_device::ptr(new cuda_linear_buffer_device(buf_size));
					cuda_util::set_with_value(
						*cuda_config,
						*buf,
						0.0F,
						static_cast<int>(buf_size / sizeof(float)),
						0);
					dst.push_back(buf);
				}
			}
			cuda_safe_call(cudaStreamSynchronize(0));

			return res;
		}

		void backward_propagation_cuda::read_data(
			const std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >& data_list,
			layer_data_list& host_data) const
		{
			for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = data_list.begin(); it != data_list.end(); ++it)
			{
				updaters.find(it->first)->second->get_data_from_device(
					it->second,
					host_data.find(it->first));
			}
		}

		std::map<std::string, std::vector<float> > backward_propagation_cuda::read_update_accum(
			const std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >& update_accum_buffers,
			const std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >& data,
			unsigned int gradient_applied_count) const
		{
			std::map<std::string, std::vector<float> > res;

			float mult = 1.0F / static_cast<float>(gradient_applied_count);

			for(std::map<std::string, std::vector<cuda_linear_buffer_device::ptr> >::const_iterator it = update_accum_buffers.begin(); it != update_accum_buffers.end(); ++it)
			{
				std::vector<float> layer_stat;

				const std::vector<cuda_linear_buffer_device::ptr>& src = data.find(it->first)->second;
				std::vector<cuda_linear_buffer_device::ptr>::const_iterator src_it = src.begin();
				for(std::vector<cuda_linear_buffer_device::ptr>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2, ++src_it)
				{
					size_t elem_count = (*src_it)->get_size() / sizeof(float);
					std::vector<double> pack((*it2)->get_size() / sizeof(double));
					cuda_safe_call(cudaMemcpy(&(*pack.begin()), **it2, (*it2)->get_size(), cudaMemcpyDeviceToHost));

					double sum = std::accumulate(
						pack.begin(), 
						pack.end(),
						0.0);

					float val = static_cast<float>(sum) * mult / static_cast<float>(elem_count);
					layer_stat.push_back(val);
				}

				res.insert(std::make_pair(it->first, layer_stat));
			}

			return res;
		}

		void backward_propagation_cuda::enqueue_apply_gradient(
			cudaStream_t stream_id,
			const std::string& layer_name,
			std::vector<cuda_linear_buffer_device::ptr>& data,
			std::vector<cuda_linear_buffer_device::ptr>& gradient,
			std::vector<cuda_linear_buffer_device::ptr>& prev_upd,
			std::vector<cuda_linear_buffer_device::ptr>& prev_upd2,
			const std::vector<float>& learning_rates,
			std::vector<cuda_linear_buffer_device::ptr>& update_accum_buffers,
			float gradient_normalizer,
			float weight_decay,
			training_momentum momentum,
			unsigned int iteration_id)
		{
			std::set<unsigned int> weight_decay_part_id_set = schema->get_layer(layer_name)->get_weight_decay_part_id_set();
			for(unsigned int part_id = 0; part_id < static_cast<unsigned int>(data.size()); ++part_id)
			{
				unsigned int elem_count = static_cast<unsigned int>(data[part_id]->get_size() / sizeof(float));
				float actual_weight_decay = (weight_decay_part_id_set.find(part_id) == weight_decay_part_id_set.end()) ? 0.0F : weight_decay;

				switch(momentum.type)
				{
				case training_momentum::no_momentum:
					cuda_util::apply_gradient(
						*cuda_config,
						*data[part_id],
						*gradient[part_id],
						*update_accum_buffers[part_id],
						learning_rates[part_id],
						gradient_normalizer,
						actual_weight_decay,
						elem_count,
						elem_count_update_accum_per_part - 1,
						stream_id);
					break;
				case training_momentum::vanilla_momentum:
					cuda_util::apply_gradient_with_vanilla_momentum(
						*cuda_config,
						*data[part_id],
						*gradient[part_id],
						*prev_upd[part_id],
						*update_accum_buffers[part_id],
						learning_rates[part_id],
						gradient_normalizer,
						actual_weight_decay,
						momentum.momentum_val,
						elem_count,
						elem_count_update_accum_per_part - 1,
						stream_id);
					break;
				case training_momentum::nesterov_momentum:
					cuda_util::apply_gradient_with_nesterov_momentum(
						*cuda_config,
						*data[part_id],
						*gradient[part_id],
						*prev_upd[part_id],
						*update_accum_buffers[part_id],
						learning_rates[part_id],
						gradient_normalizer,
						actual_weight_decay,
						momentum.momentum_val,
						elem_count,
						elem_count_update_accum_per_part - 1,
						stream_id);
					break;
				case training_momentum::adam_momentum:
					cuda_util::apply_gradient_with_adam_momentum(
						*cuda_config,
						*data[part_id],
						*gradient[part_id],
						*prev_upd[part_id],
						*prev_upd2[part_id],
						*update_accum_buffers[part_id],
						learning_rates[part_id],
						gradient_normalizer,
						actual_weight_decay,
						momentum.momentum_val,
						momentum.momentum_val2,
						elem_count,
						elem_count_update_accum_per_part - 1,
						iteration_id,
						stream_id);
					break;
				}
			}
		}

		float backward_propagation_cuda::get_max_flops() const
		{
			return cuda_config->get_flops();
		}
	}
}
