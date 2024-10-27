#include "oggstr_exporter.h"
#include "compat/resource_loader_compat.h"
#include "core/error/error_list.h"
#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/variant/variant.h"
#include "modules/vorbis/audio_stream_ogg_vorbis.h"

bool OggStrExporter::handles_import(const String &importer, const String &resource_type) const {
	// Check if the importer can handle the specified resource type
	return importer == "ogg_vorbis" || importer == "oggvorbisstr" || resource_type == "AudioStreamOGGVorbis";
}

Error OggStrExporter::get_data_from_ogg_stream(const Ref<AudioStreamOggVorbis> &sample, Vector<uint8_t> &r_data) {
	Ref<OggPacketSequence> packet_sequence = sample->get_packet_sequence();
	auto page_data = packet_sequence->get_packet_data();
	Vector<uint64_t> page_sizes;
	uint64_t total_estimated_size = 0;
	// page data is a vector of a vector of packedbytearrays
	// need to get all the sizes of the pages
	for (int i = 0; i < page_data.size(); i++) {
		uint64_t page_size = 0;
		// a page is an array of PackedByteArrays
		Array page = page_data[i];
		for (int j = 0; j < page.size(); j++) {
			int pkt_size = ((PackedByteArray)page[j]).size();
			page_size += pkt_size;
			// Header size is 26 + ceil(pkt_size / 255)
			total_estimated_size += pkt_size + (pkt_size / 255) + 100; //just fudge it
		}
		page_sizes.push_back(page_size);
	}
	uint64_t total_pagedata_body_size = [](const Vector<uint64_t> p_counts) {
		uint64_t total = 0;
		for (int i = 0; i < p_counts.size(); i++) {
			total += p_counts[i];
		}
		return total;
	}(page_sizes);
	uint64_t total_actual_body_size = 0;
	uint64_t total_acc_size = 0;
	auto playback = packet_sequence->instantiate_playback();
	ogg_packet *pkt;
	ogg_stream_state os_en;
	ogg_stream_init(&os_en, rand());

	int page_cursor = 0;
	bool reached_eos = false;
	r_data.resize_zeroed(total_estimated_size);
	while (playback->next_ogg_packet(&pkt) && !reached_eos) {
		page_cursor = playback->get_page_number();
		int page_size = page_sizes[page_cursor];
		if (pkt->e_o_s) {
			reached_eos = true;
		}
		ogg_stream_packetin(&os_en, pkt);
		ERR_FAIL_COND_V_MSG(ogg_stream_check(&os_en), ERR_FILE_CORRUPT, "Ogg stream is corrupt.");
		if (os_en.body_fill >= page_size || reached_eos) {
			if (os_en.body_fill < page_size) {
				WARN_PRINT("Reached EOS: Recorded page size is " + itos(page_size) + " but body fill is " + itos(os_en.body_fill) + ".");
			}
			ogg_page og;
			ERR_FAIL_COND_V_MSG(ogg_stream_flush_fill(&os_en, &og, page_size) == 0, ERR_FILE_CORRUPT, "Could not add page.");
			int cur_pos = total_acc_size;
			total_acc_size += og.header_len + og.body_len;
			total_actual_body_size += og.body_len;
			if (total_acc_size > r_data.size()) {
				WARN_PRINT("Resizing data to " + itos(total_acc_size));
				r_data.resize(total_acc_size);
			}
			memcpy(r_data.ptrw() + cur_pos, og.header, og.header_len);
			memcpy(r_data.ptrw() + cur_pos + og.header_len, og.body, og.body_len);
		}
	}
	page_cursor = playback->get_page_number();
	if (total_actual_body_size != total_pagedata_body_size) {
		WARN_PRINT("Actual body size" + itos(total_actual_body_size) + " does not equal the pagedata body size " + itos(total_pagedata_body_size) + ".");
	}
	// resize to the actual size
	r_data.resize(total_acc_size);

	ogg_stream_clear(&os_en);
	ERR_FAIL_COND_V_MSG(!reached_eos, ERR_FILE_CORRUPT, "All packets consumed before EOS.");
	ERR_FAIL_COND_V_MSG(page_cursor < page_sizes.size(), ERR_FILE_CORRUPT, "Did not write all pages before EOS.");
	ERR_FAIL_COND_V_MSG(r_data.size() < 4, ERR_FILE_CORRUPT, "Data is too small to be an Ogg stream.");
	ERR_FAIL_COND_V_MSG(r_data[0] != 'O' || r_data[1] != 'g' || r_data[2] != 'g' || r_data[3] != 'S', ERR_FILE_CORRUPT, "Header is missing in ogg data.");

	return OK;
}

Vector<uint8_t> OggStrExporter::get_ogg_stream_data(const Ref<AudioStreamOggVorbis> &sample) {
	Error _err = OK;
	Vector<uint8_t> data;
	_err = get_data_from_ogg_stream(sample, data);
	ERR_FAIL_COND_V_MSG(_err != OK, Vector<uint8_t>(), "Cannot convert packet sequence to raw data.");
	return data;
}

Vector<uint8_t> OggStrExporter::load_ogg_stream_data(const String &p_path, int ver_major, Error *r_err) {
	Error _err = OK;
	if (!r_err) {
		r_err = &_err;
	}
	Vector<uint8_t> data;
	if (ver_major == 0) {
		ver_major = get_ver_major(p_path);
	}
	if (ver_major == 4) {
		Ref<AudioStreamOggVorbis> sample = ResourceCompatLoader::non_global_load(p_path, "", r_err);
		ERR_FAIL_COND_V_MSG(*r_err != OK, Vector<uint8_t>(), "Cannot open resource '" + p_path + "'.");
		*r_err = get_data_from_ogg_stream(sample, data);
		ERR_FAIL_COND_V_MSG(*r_err != OK, Vector<uint8_t>(), "Cannot convert packet sequence to raw data.");
	} else {
		auto res = ResourceCompatLoader::fake_load(p_path, "", r_err);
		ERR_FAIL_COND_V_MSG(*r_err, Vector<uint8_t>(), "Cannot load resource '" + p_path + "'.");
		data = res->get("data");
	}
	return data;
}

Error OggStrExporter::_export_file(const String &dst_path, const String &res_path, int ver_major) {
	Error err = OK;
	if (ver_major == 0) {
		ver_major = get_ver_major(res_path);
	}
	Vector<uint8_t> data = load_ogg_stream_data(res_path, ver_major, &err);
	ERR_FAIL_COND_V_MSG(err != OK, err, "Failed to load Ogg stream data from " + res_path);
	err = ensure_dir(dst_path.get_base_dir());
	Ref<FileAccess> f = FileAccess::open(dst_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(f.is_null(), ERR_FILE_CANT_WRITE, "Cannot open file '" + dst_path + "' for writing.");
	f->store_buffer(data.ptr(), data.size());
	return OK;
}

Error OggStrExporter::export_file(const String &dst_path, const String &res_path) {
	return _export_file(dst_path, res_path, get_ver_major(res_path));
}

Ref<ExportReport> OggStrExporter::export_resource(const String &output_dir, Ref<ImportInfo> import_info) {
	String src_path = import_info->get_path();
	String dst_path = output_dir.path_join(import_info->get_export_dest().replace("res://", ""));
	// Implement the logic to export the Ogg Vorbis stream to the specified path
	Ref<ExportReport> report = memnew(ExportReport(import_info));
	Error err = _export_file(dst_path, src_path, import_info->get_ver_major());
	if (err != OK) {
		report->set_error(err);
		if (err == ERR_FILE_CORRUPT) {
			report->set_message("The Ogg stream is corrupt: " + src_path);
		} else {
			report->set_message("Failed to export Ogg stream: " + src_path);
		}
		ERR_FAIL_V_MSG(report, report->get_message());
	} else {
		print_verbose("Converted " + src_path + " to " + dst_path);
		report->set_saved_path(dst_path);
	}

	// Here you would add the actual export logic
	// For now, we will just simulate a successful export
	return report;
}