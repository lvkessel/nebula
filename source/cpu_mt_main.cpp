#include "config/config.h"
#include "physics_config.h"
#include "core/material.h"
#include "common/cli_params.h"
#include "common/work_pool.h"
#include "common/time_log.h"
#include "common/output_stream.h"

#include "drivers/cpu/simple_cpu_driver.h"

#include "geometry/trilist.h"
#include "geometry/octree.h"

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <thread>
#include <mutex>
#include "legacy_thomas/load_tri_file.h"
#include "legacy_thomas/load_pri_file.h"
#include "legacy_thomas/load_mat_file.h"

// Main typedefs
using geometry_t = nbl::geometry::octree<false>;
using material_t = material<scatter_physics<false>>;

using driver = nbl::drivers::simple_cpu_driver<
	scatter_physics<false>,
	intersect_t,
	geometry_t
>;

// TODO: material not really destroyed.
material_t load_material(std::string const & filename)
{
	if (filename.back() == 't')
	{
		// Old .mat file format
		return material_t(load_mat_file(filename));
	}
	else
	{
		// New HDF5 file format
		return material_t(nbl::hdf5_file(filename));
	}
}

int main(int argc, char** argv)
{
	// Settings
	real energy_threshold = 0;
	typename driver::seed_t seed = 0x14f8214e78c7e39b;
	std::string detect_filename = "stdout";

	cli_params p(argc, argv);
	p.get_optional_flag("energy-threshold", energy_threshold);
	p.get_optional_flag("seed", seed);
	p.get_optional_flag("detect-filename", detect_filename);

	const std::string usage("Usage: " + p.get_program_name() +
		" [options] <geometry.tri> <primaries.pri> [material0.mat] .. [materialN.mat]\n"
		"Options:\n"
		"\t--energy-threshold [0]\n"
		"\t--seed             [0x14f8214e78c7e39b]\n"
		"\t--detect_filename  [stdout]\n");

	// Setup time logging
	time_log timer;

	// Interpret command-line options
	std::vector<std::string> pos_flags = p.get_positional();
	if (pos_flags.size() < 3)
	{
		std::clog << usage << std::endl;
		return 1;
	}

	std::mt19937 random_generator(seed);

	// Load geometry
	std::clog << "Loading geometry..." << std::endl;
	timer.start();
	std::vector<triangle> triangles = load_tri_file(pos_flags[0]);
	timer.stop("Loading triangles");

	if (triangles.empty())
	{
		std::clog << "Error: could not load triangles!\n" << usage << std::endl;
		return 1;
	}
	// Sanity check with number of materials
	{
		int max_material = -1;
		for (triangle const & tri : triangles)
		{
			if (tri.material_in > max_material)
				max_material = tri.material_in;
			if (tri.material_out > max_material)
				max_material = tri.material_out;
		}

		if (max_material > pos_flags.size()-3)
		{
			std::clog << "Error: not enough materials provided for this geometry!\n"
				"  Expected " << (max_material+1) << " materials, " << (pos_flags.size()-2) << " provided.\n";
			return 1;
		}
		if (max_material < pos_flags.size()-3)
		{
			std::clog << "Warning: too many materials provided for this geometry!\n"
				"  Expected " << (max_material+1) << " materials, " << (pos_flags.size()-2) << " provided.\n";
		}
	}

	timer.start();
	geometry_t geometry = geometry_t::create(triangles);
	timer.stop("Building acceleration structure");


	// Load primaries
	std::clog << "Loading primary electrons..." << std::endl;
	timer.start();
	std::vector<particle> primaries; std::vector<int2> pixels;
	std::tie(primaries, pixels) = load_pri_file(pos_flags[1], geometry.AABB_min(), geometry.AABB_max());
	timer.stop("Loading primary electrons");

	if (primaries.empty())
	{
		std::clog << "Error: could not load primary electrons!\n" << usage << std::endl;
		return 1;
	}

	// The driver only accepts uint32 tags. So we make a map: simulation tag is
	// the index of the primary particle in the "primaries" / "pixels" array.
	std::vector<uint32_t> gpu_tags(primaries.size());
	std::iota(gpu_tags.begin(), gpu_tags.end(), 0); // Fill with 0, 1, ... tags.size()-1

	// This manages the work to be done (thread-safe).
	work_pool pool(primaries.data(), gpu_tags.data(), primaries.size());


	// Load materials
	std::clog << "Loading materials..." << std::endl;
	timer.start();
	std::vector<material_t> materials;
	for (size_t parameter_idx = 2; parameter_idx < pos_flags.size(); ++parameter_idx)
		materials.push_back(load_material(pos_flags[parameter_idx]));
	timer.stop("Loading materials");

	intersect_t inter;

	// Print debug data
	std::clog << "\n"
		<< "Loaded " << triangles.size() << " triangles.\n"
		<< "  min = {" << geometry.AABB_min().x << ", " << geometry.AABB_min().y << ", " << geometry.AABB_min().z << "}\n"
		<< "  max = {" << geometry.AABB_max().x << ", " << geometry.AABB_max().y << ", " << geometry.AABB_max().z << "}\n"
		<< "Loaded " << primaries.size() << " primaries.\n"
		<< "Loaded " << materials.size() << " materials.\n\n" << std::flush;

	// Prepare output file
	output_stream out_file(detect_filename);

	// Simulation loop
	auto sim_loop = [&pool, &out_file, &pixels,
		&geometry, &inter, &materials, energy_threshold](uint64_t seed)
	{
		driver d(geometry, inter, materials, energy_threshold, seed);
		output_buffer buff(out_file, 1024*(7*sizeof(float) + 2*sizeof(int)));

		for (;;)
		{
			// Push new particles
			auto work_data = pool.get_work(1);

			if (std::get<2>(work_data) == 0)
				break;

			auto particles_pushed = d.push(
				std::get<0>(work_data),  // particle*
				std::get<1>(work_data),  // tag*
				std::get<2>(work_data)); // number

			// Simulate a little
			d.simulate_to_end();

			// Flush output data
			d.flush_detected([&buff,&pixels](particle p, uint32_t t)
			{
				buff.add(std::array<float, 7>{
					p.pos.x, p.pos.y, p.pos.z,
					p.dir.x, p.dir.y, p.dir.z, p.kin_energy});
				buff.add(std::array<int, 2>{
					pixels[t].x, pixels[t].y});
			});
		}

		buff.flush();
	};

	// Simulation
	const auto n_threads = std::thread::hardware_concurrency();
	std::clog << "Creating " << n_threads << " CPU drivers" << std::endl;
	std::vector<std::thread> threads;

	timer.start();
	for (unsigned int i = 0; i < n_threads; ++i)
		threads.push_back(std::thread(sim_loop, random_generator()));

	// Progress indicator
	for (;;)
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
		auto primaries_to_go = pool.get_primaries_to_go();
		std::clog << " \rProgress "
			<< std::fixed << std::setprecision(2) << 100 * (1 - ((double)primaries_to_go / primaries.size())) << "%";
		if (primaries_to_go == 0)
			break;
	}

	for (auto& t : threads)
		t.join();

	timer.stop("Simulation");

	std::clog << "\n\n";
	timer.print(std::clog);
	return 0;
}
