#include <cstdint>

#define FMT_UNICODE 0
#include <spdlog/spdlog.h>

#include "config/arg_parser.hpp"
#include "config/config.hpp"

#include "utilities.hpp"

#include "emulator/emulator.hpp"
#include "win_process/win_process.hpp"

#include "portable_executable/file.hpp"
#include "portable_executable/image.hpp"

#include "vmprotect/vmp.hpp"
#include "vmprotect/vmp_iat.hpp"
#include "vmprotect/vmp_image.hpp"
#include "vmprotect/vmp_utilities.hpp"

std::int32_t main(const std::int32_t argc, const char** argv)
{
	const auto& arg_parser_ctx = config::arg_parser::parse(argc, argv);

	if (!arg_parser_ctx)
	{
		spdlog::error(arg_parser_ctx.error());

		return EXIT_FAILURE;
	}

	config::context_t config_ctx;

	try
	{
		config_ctx = config::parse_toml(*arg_parser_ctx);
	}
	catch (std::exception& error)
	{
		spdlog::error(error.what());

		return EXIT_FAILURE;
	}

	const auto& process_id = utilities::find_process_id(config_ctx.process_name);

	if (!process_id)
	{
		spdlog::error(process_id.error());

		return EXIT_FAILURE;
	}

	win_process_t win_process(*process_id);

	if (!win_process.attach())
	{
		spdlog::error("failed to attach to process id {}", *process_id);

		return EXIT_FAILURE;
	}

	const auto& module = win_process.find_module(config_ctx.module_name);

	if (!module)
	{
		spdlog::error(module.error());

		return EXIT_FAILURE;
	}

	portable_executable::file_t file(module->path);

	if (!file.load())
	{
		spdlog::error("failed to load file from path {}", module->path.string());

		return EXIT_FAILURE;
	}

	const auto& image = file.image();

	bool is_x64;

	switch (image->nt_headers()->file_header.machine)
	{
		case portable_executable::machine_id_t::amd64:
			is_x64 = true;
			break;

		case portable_executable::machine_id_t::i386:
			is_x64 = false;
			break;

		default:
			spdlog::error("invalid machine id. this software only supports x86 and x86_64 portable executables!");
			return EXIT_FAILURE;
	}

	try
	{
		vmp::construct_context(is_x64);

		vmp::compute_sections(config_ctx.vmp_sections, module->address, image);
	}
	catch (std::exception& error)
	{
		spdlog::error(error.what());

		return EXIT_FAILURE;
	}

	std::vector<std::pair<std::uintptr_t, std::vector<std::uint8_t>>> map_sections = { };

	std::vector<std::uintptr_t> import_calls = { };

	{
		const portable_executable::section_header_t* text_section = image->find_section(".text");

		if (!text_section)
		{
			spdlog::error("failed to find section .text");

			return EXIT_FAILURE;
		}

		std::vector<std::uint8_t> temp_buffer(text_section->virtual_size);

		const std::uintptr_t address = module->address + text_section->virtual_address;

		if (!win_process.read(address, temp_buffer.data(), temp_buffer.size()))
		{
			spdlog::error("failed to read from remote process");

			return EXIT_FAILURE;
		}

		import_calls = vmp::utilities::scan_import_calls(address, temp_buffer);

		map_sections.emplace_back(address, temp_buffer);
	}

	for (const auto& vmp_section : config_ctx.vmp_sections)
	{
		const portable_executable::section_header_t* section_header = image->find_section(vmp_section);

		if (!section_header)
		{
			spdlog::error("failed to find section {}", vmp_section);

			return EXIT_FAILURE;
		}

		std::vector<std::uint8_t> temp_buffer(section_header->virtual_size);

		const std::uintptr_t address = module->address + section_header->virtual_address;

		if (!win_process.read(address, temp_buffer.data(), temp_buffer.size()))
		{
			spdlog::error("failed to read from remote process");

			return EXIT_FAILURE;
		}

		map_sections.emplace_back(address, temp_buffer);
	}

	try
	{
		vmp::map_sections(map_sections);
		vmp::process_import_calls(import_calls, module->address);
	}
	catch (std::exception& error)
	{
		spdlog::error(error.what());

		return EXIT_FAILURE;
	}

	const auto mapped_modules = win_process.modules_local_mapped();

	if (!mapped_modules)
	{
		spdlog::error(mapped_modules.error());

		return EXIT_FAILURE;
	}

	spdlog::info("starting iat rebuild...");

	vmp::iat_t iat;

	for (const auto& import : vmp::context->imports)
	{
		bool found = false;

		for (const auto& [pe, remote_image_base, module_name] : *mapped_modules)
		{
			const std::uintptr_t possible_import_va = import.import_address - remote_image_base;

			for (	const auto mapped_image = pe->image();
					const auto& [export_name, export_va, _] : mapped_image->exports())
			{
				if (possible_import_va == export_va)
				{
					iat.add_import(module_name, export_name);

					found = true;
				}

				if (found)
				{
					break;
				}
			}

			if (found)
			{
				break;
			}
		}
	}

	vmp::image_t vmp_image(module->address);

	try
	{
		vmp_image.initialize_memory_pe(module->size, &win_process);

		iat.reconstruct(&vmp_image, config_ctx.iat_section_name);

		vmp_image.dump_to_fs(config_ctx.dump_path);
	}
	catch (std::exception& error)
	{
		spdlog::error(error.what());

		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
