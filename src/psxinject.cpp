//
// PSXInject - Replace files in a PlayStation 1 disc image
//
// Copyright (C) 2014 Christian Bauer <www.cebix.net>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
//

#include <stdint.h>

#include <cdio/cdio.h>
#include <cdio/cd_types.h>
#include <cdio/iso9660.h>
#include <cdio/logging.h>
#include <cdio/bytesex.h>

extern "C" {
#include <libvcd/info.h>
#include <libvcd/sector.h>
}

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
using boost::format;

#include <exception>
#include <fstream>
#include <iostream>
#include <string>
using namespace std;


#define TOOL_VERSION "PSXInject 2.0"


// Print usage information and exit.
static void usage(const char * progname, int exitcode = 0, const string & error = "")
{
	cout << "Usage: " << boost::filesystem::path(progname).filename().native() << " [OPTION...] <input>[.bin/cue] <repl_file_path> <new_file>" << endl;
	cout << "  -v, --verbose                   Be verbose" << endl;
	cout << "  -V, --version                   Display version information and exit" << endl;
	cout << "  -?, --help                      Show this help message" << endl;

	if (!error.empty()) {
		cerr << endl << "Error: " << error << endl;
	}

	exit(exitcode);
}


// Main program
int main(int argc, char ** argv)
{
	// Parse command line arguments
	boost::filesystem::path imagePath;
	string replFilePath;
	boost::filesystem::path newFileName;

	for (int i = 1; i < argc; ++i) {
		string arg = argv[i];

		if (arg == "--version" || arg == "-V") {
			cout << TOOL_VERSION << endl;
			return 0;
		} else if (arg == "--verbose" || arg == "-v") {
			cdio_loglevel_default = CDIO_LOG_INFO;
		} else if (arg == "--help" || arg == "-?") {
			usage(argv[0]);
		} else if (arg[0] == '-') {
			usage(argv[0], 64, "Invalid option '" + arg + "'");
		} else {
			if (imagePath.empty()) {
				imagePath = arg;
			} else if (replFilePath.empty()) {
				replFilePath = arg;
			} else if (newFileName.empty()) {
				newFileName = arg;
			} else {
				usage(argv[0], 64, "Unexpected extra argument '" + arg + "'");
			}
		}
	}

	if (imagePath.empty()) {
		usage(argv[0], 64, "No image file specified");
	} else if (replFilePath.empty()) {
		usage(argv[0], 64, "No file to be replaced specified");
	} else if (newFileName.empty()) {
		usage(argv[0], 64, "No new file specified");
	}

	try {

		// Open the image file
		if (imagePath.extension().empty()) {
			imagePath.replace_extension(".bin");
		}

		CdIo_t * image = cdio_open(imagePath.c_str(), DRIVER_BINCUE);
		if (image == NULL) {
			throw runtime_error((format("Error opening input image %1%, or image has wrong type") % imagePath).str());
		}

		// Check the track type
		track_t firstTrack = cdio_get_first_track_num(image);
		cdio_info("First track = %d", firstTrack);
		if (firstTrack == CDIO_INVALID_TRACK) {
			throw runtime_error("Cannot determine first track number");
		}

		track_format_t trackFormat = cdio_get_track_format(image, firstTrack);
		cdio_info("Track format = %d", trackFormat);
		if (trackFormat != TRACK_FORMAT_DATA && trackFormat != TRACK_FORMAT_XA) {
			throw runtime_error((format("First track (%1%) is not a data track") % firstTrack).str());
		}

		bool imageIsMode2 = (trackFormat == TRACK_FORMAT_XA);

		// Find the file in the image
		if (!iso9660_fs_read_superblock(image, ISO_EXTENSION_NONE)) {
			throw runtime_error("Error reading ISO 9660 volume information");
		}

		iso9660_stat_t * stat = iso9660_fs_stat(image, (replFilePath + ";1").c_str());
		if (!stat) {
			throw runtime_error((format("Cannot find '%1%' in image") % replFilePath).str());
		}
		if (stat->type != iso9660_stat_s::_STAT_FILE) {
			throw runtime_error((format("'%1%' does not refer to a file") % replFilePath).str());
		}

		bool fileIsForm2 = false;
		if (stat->b_xa) {
			uint16_t attr = uint16_from_be(stat->xa.attributes);
			if (attr & (XA_ATTR_MODE2FORM2 | XA_ATTR_INTERLEAVED)) {
				fileIsForm2 = true;
			}
		}

		uint32_t extent = stat->lsn;
		uint32_t maxSectors = stat->secsize;
		cdio_info("'%s' (form %d) found at LBN %d, length = %d sectors (%d bytes)", replFilePath.c_str(), fileIsForm2 ? 2 : 1, extent, maxSectors, stat->size);

		// Check the new file
		uintmax_t newSize = boost::filesystem::file_size(newFileName);
		size_t blockSize = fileIsForm2 ? M2RAW_SECTOR_SIZE : ISO_BLOCKSIZE;

		if (fileIsForm2) {
			if (!imageIsMode2) {
				throw runtime_error((format("'%1%' is a form 2 file but '%2%' is not a raw mode 2 image")
				                     % replFilePath % imagePath).str());
			}

			if (newSize % blockSize != 0) {
				throw runtime_error((format("'%1%' is a form 2 file but the size of %2% is not a multiple of %3% bytes")
				                     % replFilePath % newFileName % blockSize).str());
			}
		}

		uint32_t numSectors = (newSize + blockSize - 1) / blockSize;
		if (numSectors == 0) {
			numSectors = 1;  // empty files use one sector
		}

		if (numSectors > maxSectors) {
			throw runtime_error((format("%1% would require %2% sectors but there is only room for %3% sectors (%4% bytes)")
			                     % newFileName % numSectors % maxSectors % (maxSectors * blockSize)).str());
		}

		// Find the directory in the image
		string dirPath = boost::filesystem::path(replFilePath).parent_path().generic_string();
		if (dirPath.empty()) {
			dirPath = "/";
		}

		iso9660_stat_t * dirStat = iso9660_fs_stat(image, dirPath.c_str());
		if (!dirStat) {
			throw runtime_error((format("Cannot find '%1%' in image") % dirPath).str());
		}
		if (dirStat->type != iso9660_stat_s::_STAT_DIR) {
			throw runtime_error((format("'%1%' does not refer to a directory") % dirPath).str());
		}

		// Scan the directory for the file record
		uint32_t dirSector = dirStat->lsn;
		uint32_t numDirSectors = dirStat->secsize;
		size_t dirOffset = 0;
		bool isLastDirSector = false;

		string searchName = boost::filesystem::path(replFilePath).filename().generic_string() + ";1";
		bool found = false;

		uint8_t dirBuffer[ISO_BLOCKSIZE];

		for (size_t sector = 0; sector < numDirSectors && !found; ++sector) {
			if (sector == numDirSectors - 1) {
				isLastDirSector = true;
			}

			driver_return_code_t r = cdio_read_data_sectors(image, dirBuffer, dirSector + sector, ISO_BLOCKSIZE, 1);
			if (r != DRIVER_OP_SUCCESS) {
				throw runtime_error((format("Error reading sector %1% of image file: %2%") % (dirSector + sector) % cdio_driver_errmsg(r)).str());
			}

			size_t offset = 0;
			while (offset < ISO_BLOCKSIZE) {

				// Get record length
				size_t recLen = dirBuffer[offset];
				if (recLen == 0) {
					offset++;  // empty padding at end of sector
					continue;
				}

				// Get record type, skip directories
				uint8_t type = dirBuffer[offset + offsetof(iso9660_dir_t, file_flags)];
				if (type & ISO_DIRECTORY) {
					offset += recLen;
					continue;
				}

				// Compare file name
				size_t nameLen = dirBuffer[offset + offsetof(iso9660_dir_t, filename)];
				if (string((char *) dirBuffer + offset + offsetof(iso9660_dir_t, filename) + 1, nameLen) == searchName) {

					// Found it
					dirSector = dirSector + sector;
					dirOffset = offset;
					found = true;
					break;
				}

				offset += recLen;
			}
		}

		if (!found) {
			throw runtime_error((format("'%1%' not found in directory '%2%'") % searchName % dirPath).str());
		}

		// Reopen the image file for writing
		cdio_destroy(image);

		imagePath.replace_extension(".bin");
		fstream writeImage(imagePath.c_str(), fstream::in | fstream::out | fstream::binary);

		// Read the new file and inject it
		ifstream file(newFileName.c_str(), ifstream::in | ifstream::binary);
		if (!file) {
			throw runtime_error((format("Cannot open file %1%") % newFileName).str());
		}

		char data[M2RAW_SECTOR_SIZE];
		uint8_t buffer[CDIO_CD_FRAMESIZE_RAW];
		uint32_t outputBlockSize = imageIsMode2 ? CDIO_CD_FRAMESIZE_RAW : ISO_BLOCKSIZE;

		for (size_t sector = 0; sector < numSectors; ++sector) {
			memset(data, 0, sizeof(data));
			file.read(data, blockSize);

			writeImage.seekp((extent + sector) * outputBlockSize);

			if (imageIsMode2) {
				uint8_t subMode = SM_DATA;
				if (sector == numSectors - 1) {
					subMode |= (SM_EOF | SM_EOR);  // last sector
				}

				if (fileIsForm2) {
					_vcd_make_mode2(buffer, data + CDIO_CD_SUBHEADER_SIZE, extent + sector, data[0], data[1], data[2], data[3]);
				} else {
					_vcd_make_mode2(buffer, data, extent + sector, 0, 0, subMode, 0);
				}

				writeImage.write((char *)buffer, CDIO_CD_FRAMESIZE_RAW);
			} else {
				writeImage.write(data, ISO_BLOCKSIZE);
			}
		}

		// Replace the file size in the directory record and write it back
		if (fileIsForm2) {
			*reinterpret_cast<iso733_t *>(dirBuffer + dirOffset + 10) = to_733(numSectors * ISO_BLOCKSIZE);
		} else {
			*reinterpret_cast<iso733_t *>(dirBuffer + dirOffset + 10) = to_733(newSize);
		}

		writeImage.seekp(dirSector * outputBlockSize);
		if (imageIsMode2) {
			uint8_t subMode = SM_DATA;
			if (isLastDirSector) {
				subMode |= (SM_EOF | SM_EOR);  // last sector
			}
			_vcd_make_mode2(buffer, dirBuffer, dirSector, 0, 0, subMode, 0);
			writeImage.write((char *)buffer, CDIO_CD_FRAMESIZE_RAW);
		} else {
			writeImage.write((char *)dirBuffer, ISO_BLOCKSIZE);
		}

		cout << "File '" << replFilePath << "' replaced in " << imagePath << endl;
		cdio_info("Done.");

	} catch (const std::exception & e) {
		cerr << e.what() <<endl;
		return 1;
	}

	return 0;
}
