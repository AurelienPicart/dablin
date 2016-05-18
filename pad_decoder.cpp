/*
    DABlin - capital DAB experience
    Copyright (C) 2015 Stefan Pöschel

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pad_decoder.h"


// --- XPAD_CI -----------------------------------------------------------------
const size_t XPAD_CI::lens[] = {4, 6, 8, 12, 16, 24, 32, 48};

int XPAD_CI::GetContinuedLastCIType(int last_ci_type) {
	switch(last_ci_type) {
	case 1:		// Data group length indicator
		return 1;
	case 2:		// Dynamic Label segment
	case 3:
		return 3;
	case 12:	// MOT, X-PAD data group
	case 13:
		return 13;
	case -1:
	default:
		return -1;
	}
}


// --- PADDecoder -----------------------------------------------------------------
PADDecoder::PADDecoder(PADDecoderObserver *observer) {
	this->observer = observer;
	Reset();
}

void PADDecoder::Reset() {
	last_xpad_ci.Reset();
	{
		std::lock_guard<std::mutex> lock(data_mutex);

		dl.Reset();
	}

	dl_decoder.Reset();
	dgli_decoder.Reset();
}

void PADDecoder::Process(const uint8_t *xpad_data, size_t xpad_len, uint16_t fpad) {
	xpad_cis_t xpad_cis;
	size_t xpad_cis_len = -1;

	int fpad_type = fpad >> 14;
	int xpad_ind = (fpad & 0x3000) >> 12;
	bool ci_flag = fpad & 0x0002;

	// build CI list
	if(fpad_type == 0b00) {
		if(ci_flag) {
			switch(xpad_ind) {
			case 0b01:	// short X-PAD
				xpad_cis_len = 1;
				xpad_cis.push_back(XPAD_CI(3, xpad_data[0] & 0x1F));
				break;
			case 0b10:	// variable size X-PAD
				xpad_cis_len = 0;
				for(size_t i = 0; i < 4; i++) {
					uint8_t ci_raw = xpad_data[i];
					xpad_cis_len++;

					// break on end marker
					if((ci_raw & 0x1F) == 0x00)
						break;

					xpad_cis.push_back(XPAD_CI(ci_raw));
				}
				break;
			}
		} else {
			switch(xpad_ind) {
			case 0b01:	// short X-PAD
			case 0b10:	// variable size X-PAD
				// if there is a last CI, append it
				if(last_xpad_ci.type != -1) {
					xpad_cis_len = 0;
					xpad_cis.push_back(last_xpad_ci);
				}
				break;
			}
		}
	}

	// reset last CI
	last_xpad_ci.Reset();


	// process CIs
//	fprintf(stderr, "PADDecoder: -----\n");
	if(xpad_cis.empty())
		return;

	size_t xpad_offset = xpad_cis_len;
	for(xpad_cis_t::const_iterator it = xpad_cis.cbegin(); it != xpad_cis.cend(); it++) {
		// abort, if Data Subfield out of X-PAD
		if(xpad_offset + it->len > xpad_len)
			return;

		// handle Data Subfield
		switch(it->type) {
		case 1:		// Data Group Length Indicator
			dgli_decoder.ProcessDataSubfield(ci_flag, xpad_data + xpad_offset, it->len);

			// TODO: process result

			break;
		case 2:		// Dynamic Label segment
		case 3:
			// if new label available, append it
			if(dl_decoder.ProcessDataSubfield(it->type == 2, xpad_data + xpad_offset, it->len)) {
				{
					std::lock_guard<std::mutex> lock(data_mutex);

					dl = dl_decoder.GetLabel();
				}
				observer->PADChangeDynamicLabel();
			}
			break;
		}
//		fprintf(stderr, "PADDecoder: Data Subfield: type: %2d, len: %2zu\n", it->type, it->len);

		xpad_offset += it->len;
	}

	// set last CI
	last_xpad_ci.len = xpad_offset;
	last_xpad_ci.type = XPAD_CI::GetContinuedLastCIType(xpad_cis.back().type);
}


// --- DataGroup -----------------------------------------------------------------
DataGroup::DataGroup(size_t dg_size_max) {
	dg_raw.resize(dg_size_max);
	Reset();
}

void DataGroup::Reset() {
	dg_size = 0;
	dg_size_needed = 0;
}

bool DataGroup::ProcessDataSubfield(bool start, const uint8_t *data, size_t len) {
	if(start) {
		Reset();
	} else {
		// ignore Data Group continuation without previous start
		if(dg_size == 0)
			return false;
	}

	// abort, if needed size already reached (except needed size not yet set)
	if(dg_size >= dg_size_needed && dg_size_needed != 0)
		return false;

	// abort, if maximum size already reached
	if(dg_size == dg_raw.size())
		return false;

	// append Data Subfield
	size_t copy_len = dg_raw.size() - dg_size;
	if(len < copy_len)
		copy_len = len;
	memcpy(&dg_raw[dg_size], data, copy_len);
	dg_size += copy_len;

	// abort, if needed size not yet reached
	if(dg_size < dg_size_needed)
		return false;

	return DecodeDataGroup();
}

bool DataGroup::EnsureDataGroupSize(size_t desired_dg_size) {
	if(dg_size < desired_dg_size) {
		dg_size_needed = desired_dg_size;
		return false;
	}
	return true;
}

bool DataGroup::CheckCRC(size_t len) {
	// ensure needed size reached
	if(dg_size < len + CRC_LEN)
		return false;

	uint16_t crc_stored = dg_raw[len] << 8 | dg_raw[len + 1];
	uint16_t crc_calced = CalcCRC::CalcCRC_CRC16_CCITT.Calc(&dg_raw[0], len);
	return crc_stored == crc_calced;
}


// --- DGLIDecoder -----------------------------------------------------------------
void DGLIDecoder::Reset() {
	DataGroup::Reset();

	dgli_len = 0;
}

bool DGLIDecoder::DecodeDataGroup() {
	// DG len + CRC
	if(!EnsureDataGroupSize(2 + CRC_LEN))
		return false;

	// abort on invalid CRC
	if(!CheckCRC(2)) {
		DataGroup::Reset();
		return false;
	}

	dgli_len = (dg_raw[0] & 0x3F) << 8 | dg_raw[1];

	DataGroup::Reset();

//	fprintf(stderr, "DGLIDecoder: dgli_len: %zu\n", dgli_len);

	return true;
}

size_t DGLIDecoder::GetDGLILen() {
	size_t result = dgli_len;
	dgli_len = 0;
	return result;
}


// --- DynamicLabelDecoder -----------------------------------------------------------------
void DynamicLabelDecoder::Reset() {
	DataGroup::Reset();

	dl_sr.Reset();
	label.Reset();
}

bool DynamicLabelDecoder::DecodeDataGroup() {
	// at least prefix + CRC
	if(!EnsureDataGroupSize(2 + CRC_LEN))
		return false;

	bool command = dg_raw[0] & 0x10;

	size_t field_len = 0;
	bool cmd_remove_label = false;

	// handle command/segment
	if(command) {
		switch(dg_raw[0] & 0x0F) {
		case DL_CMD_REMOVE_LABEL:
			cmd_remove_label = true;
			break;
		default:
			// ignore command
			DataGroup::Reset();
			return false;
		}
	} else {
		field_len = (dg_raw[0] & 0x0F) + 1;
	}

	size_t real_len = 2 + field_len;

	if(!EnsureDataGroupSize(real_len + CRC_LEN))
		return false;

	// abort on invalid CRC
	if(!CheckCRC(real_len)) {
		DataGroup::Reset();
		return false;
	}

	// on Remove Label command, display empty label
	if(cmd_remove_label) {
		label.raw.clear();
		label.charset = 0;	// EBU Latin based (though it doesn't matter)
		return true;
	}

	// create new segment
	DL_SEG dl_seg;
	memcpy(dl_seg.prefix, &dg_raw[0], 2);
	dl_seg.chars.insert(dl_seg.chars.begin(), dg_raw.begin() + 2, dg_raw.begin() + 2 + field_len);

	DataGroup::Reset();

//	fprintf(stderr, "DynamicLabelDecoder: segnum %d, toggle: %s, chars_len: %2d%s\n", dl_seg.SegNum(), dl_seg.Toggle() ? "Y" : "N", dl_seg.chars.size(), dl_seg.Last() ? " [LAST]" : "");

	// try to add segment
	if(!dl_sr.AddSegment(dl_seg))
		return false;

	// append new label
	label.raw = dl_sr.label_raw;
	label.charset = dl_sr.dl_segs[0].prefix[1] >> 4;
	return true;
}


// --- DL_SEG_REASSEMBLER -----------------------------------------------------------------
void DL_SEG_REASSEMBLER::Reset() {
	dl_segs.clear();
	label_raw.clear();
}

bool DL_SEG_REASSEMBLER::AddSegment(DL_SEG &dl_seg) {
	dl_segs_t::const_iterator it;

	// if there are already segments with other toggle value in cache, first clear it
	it = dl_segs.cbegin();
	if(it != dl_segs.cend() && it->second.Toggle() != dl_seg.Toggle())
		dl_segs.clear();

	// if the segment is already there, abort
	it = dl_segs.find(dl_seg.SegNum());
	if(it != dl_segs.cend())
		return false;

	// add segment
	dl_segs[dl_seg.SegNum()] = dl_seg;

	// check for complete label
	return CheckForCompleteLabel();
}

bool DL_SEG_REASSEMBLER::CheckForCompleteLabel() {
	dl_segs_t::const_iterator it;

	// check if all segments are in cache
	int segs = 0;
	for(int i = 0; i < 8; i++) {
		it = dl_segs.find(i);
		if(it == dl_segs.cend())
			return false;

		segs++;

		if(it->second.Last())
			break;

		if(i == 7)
			return false;
	}

	// append complete label
	label_raw.clear();
	for(int i = 0; i < segs; i++)
		label_raw.insert(label_raw.end(), dl_segs[i].chars.begin(), dl_segs[i].chars.end());

//	std::string label((const char*) &label_raw[0], label_raw.size());
//	fprintf(stderr, "DL_SEG_REASSEMBLER: new label: '%s'\n", label.c_str());
	return true;
}
