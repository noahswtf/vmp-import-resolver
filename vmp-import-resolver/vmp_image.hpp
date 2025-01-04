#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <expected>
#include <filesystem>

#include "portable_executable/image.hpp"
#include "portable_executable/section_header.hpp"

class win_process_t;

class vmp_image_t
{
	std::uintptr_t m_image_base = 0;

	std::vector<std::uint8_t> m_buffer;

	portable_executable::image_t* image();

public:
	explicit vmp_image_t(std::uintptr_t image_base);

	void initialize_memory_pe(std::size_t image_size, const win_process_t* win_process);

	std::expected<portable_executable::section_header_t*, std::string> add_section(std::string_view name, std::uint32_t size, portable_executable::section_characteristics_t section_characteristics);

	void dump_to_fs(const std::filesystem::path& fs_path);
};
