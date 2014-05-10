//
// PSXBuild - Build a PlayStation 1 disc image
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
#include <cdio/iso9660.h>
#include <cdio/bytesex.h>

extern "C" {
#include <libvcd/sector.h>
}

#include <string.h>
#include <time.h>

#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/regex.hpp>
#include <boost/scoped_array.hpp>
using boost::format;

#include <algorithm>
#include <exception>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <vector>
using namespace std;


#define TOOL_VERSION "PSXBuild 2.0"


struct FileNode;
struct DirNode;


// Mode 2 raw sector buffer
static char buffer[CDIO_CD_FRAMESIZE_RAW];

// Empty Form 2 sector
static const uint8_t emptySector[M2F2_SECTOR_SIZE] = {0};

// Maximum number of sectors in an image
const uint32_t MAX_ISO_SECTORS = 74 * 60 * 75;  // 74 minutes


// Create an ISO long-format time structure from an ISO8601-like string
static void parse_ltime(const string & s, iso9660_ltime_t & t)
{
	static const boost::regex timeSpec("(\\d{4})-(\\d{2})-(\\d{2})\\s+(\\d{2}):(\\d{2}):(\\d{2})\\.(\\d{2})\\s+(\\d+)");
	boost::smatch m;

	if (!boost::regex_match(s, m, timeSpec)) {
		throw runtime_error((format("'%1%' is not a valid date/time specification") % s).str());
	}

	t.lt_year[0] = m.str(1)[0];
	t.lt_year[1] = m.str(1)[1];
	t.lt_year[2] = m.str(1)[2];
	t.lt_year[3] = m.str(1)[3];

	t.lt_month[0] = m.str(2)[0];
	t.lt_month[1] = m.str(2)[1];

	t.lt_day[0] = m.str(3)[0];
	t.lt_day[1] = m.str(3)[1];

	t.lt_hour[0] = m.str(4)[0];
	t.lt_hour[1] = m.str(4)[1];

	t.lt_minute[0] = m.str(5)[0];
	t.lt_minute[1] = m.str(5)[1];

	t.lt_second[0] = m.str(6)[0];
	t.lt_second[1] = m.str(6)[1];

	t.lt_hsecond[0] = m.str(7)[0];
	t.lt_hsecond[1] = m.str(7)[1];

	try {
		t.lt_gmtoff = boost::lexical_cast<int>(m[8]);
	} catch (const boost::bad_lexical_cast &) {
		throw runtime_error((format("'%1%' is not a valid GMT offset specification") % m[8]).str());
	}
}


// Set an ISO long-format time structure to an empty value.
static void zero_ltime(iso9660_ltime_t & t)
{
	memset(&t, '0', sizeof(t));
	t.lt_gmtoff = 0;
}


// Base class for filesystem tree visitors
class Visitor {
public:
	Visitor() { }
	virtual ~Visitor() { }

	virtual void visit(FileNode &) { }
	virtual void visit(DirNode &) { }
};


// Base class for filesystem node
struct FSNode {
	FSNode(const string & name_, const boost::filesystem::path & path_, DirNode * parent_, uint32_t startSector_ = 0)
		: parent(parent_), name(name_), path(path_), firstSector(0), numSectors(0), requestedStartSector(startSector_) { }

	virtual ~FSNode() { }

	// Pointer to parent directory node (NULL if root)
	DirNode * parent;

	// List of child nodes
	vector<FSNode *> children;

	// List of child nodes sorted by name
	vector<FSNode *> sortedChildren;

	// Node name
	string name;

	// Path to item in host filesystem
	boost::filesystem::path path;

	// First logical sector number
	uint32_t firstSector;

	// Size in sectors
	uint32_t numSectors;

	// First logical sector number requested in catalog (0 = don't care)
	uint32_t requestedStartSector;

	// Polymorphic helper method for accepting a visitor
	virtual void accept(Visitor &) = 0;

	// Pre-order tree traversal
	void traverse(Visitor & v);

	// Pre-order tree traversal, children sorted by name
	void traverseSorted(Visitor & v);

	// Breadth-first tree traversal, children sorted by name
	void traverseBreadthFirstSorted(Visitor & v);
};


// Functor for sorting a container of FSNode pointers by name
struct CmpByName {
	bool operator()(const FSNode * lhs, const FSNode * rhs)
	{
		return lhs->name < rhs->name;
	}
};


// File (leaf) node
struct FileNode : public FSNode {
	FileNode(const string & name_, const boost::filesystem::path & path_, DirNode * parent_, uint32_t startSector_ = 0, bool isForm2_ = false)
		: FSNode(name_, path_, parent_, startSector_), isForm2(isForm2_)
	{
		// Check for the existence of the file and obtain its size
		size = boost::filesystem::file_size(path);

		// Calculate the number of sectors in the file extent
		size_t blockSize = isForm2 ? M2RAW_SECTOR_SIZE : ISO_BLOCKSIZE;
		numSectors = (size + blockSize - 1) / blockSize;

		if (numSectors == 0) {
			numSectors = 1;  // empty files use one sector
		}
	}

	// Size in bytes
	uint32_t size;

	// True if form 2 file
	bool isForm2;

	void accept(Visitor & v) { v.visit(*this); }
};


// Directory node
struct DirNode : public FSNode {
	DirNode(const string & name_, const boost::filesystem::path & path_, DirNode * parent_ = NULL, uint32_t startSector_ = 0)
		: FSNode(name_, path_, parent_, startSector_), data(NULL), recordNumber(0) { }

	// Pointer to directory extent data
	uint8_t * data;

	// Record number of directory in path table
	uint16_t recordNumber;

	void accept(Visitor & v) { v.visit(*this); }
};


// Pre-order tree traversal
void FSNode::traverse(Visitor & v)
{
	accept(v);

	for (vector<FSNode *>::const_iterator i = children.begin(); i != children.end(); ++i) {
		(*i)->traverse(v);
	}
}


// Pre-order tree traversal, children sorted by name
void FSNode::traverseSorted(Visitor & v)
{
	accept(v);

	for (vector<FSNode *>::const_iterator i = sortedChildren.begin(); i != sortedChildren.end(); ++i) {
		(*i)->traverseSorted(v);
	}
}


// Breadth-first tree traversal, children sorted by name
void FSNode::traverseBreadthFirstSorted(Visitor & v)
{
	queue<FSNode *> q;
	q.push(this);

	while (!q.empty()) {
		FSNode * node = q.front();
		q.pop();

		node->accept(v);

		for (vector<FSNode *>::const_iterator i = node->sortedChildren.begin(); i != node->sortedChildren.end(); ++i) {
			q.push(*i);
		}
	}
}


// Data from catalog file
struct Catalog {
	Catalog() : root(NULL), defaultUID(0), defaultGID(0)
	{
		zero_ltime(creationDate);
		zero_ltime(modificationDate);
		zero_ltime(expirationDate);
		zero_ltime(effectiveDate);
	}

	// Name of file containing system area data
	string systemAreaFile;

	// Volume information
	string systemID;
	string volumeID;
	string volumeSetID;
	string publisherID;
	string preparerID;
	string applicationID;
	string copyrightFileID;
	string abstractFileID;
	string bibliographicFileID;

	// Dates
	iso9660_ltime_t creationDate;
	iso9660_ltime_t modificationDate;
	iso9660_ltime_t expirationDate;
	iso9660_ltime_t effectiveDate;

	// Default user/group IDs
	uint16_t defaultUID;
	uint16_t defaultGID;

	// Root directory of the filesystem tree
	DirNode * root;
};


// Read the next non-empty line from a file, stripping leading and
// trailing whitespace. Returns an empty string if the end of the file was
// reached or an error occurred.
static string nextline(ifstream & file)
{
	string line;

	while (line.empty() && file) {
		getline(file, line);
		boost::trim(line);
	}

	return line;
}


// Check that the given string only consists of d-characters.
static void checkDString(const string & s, const string & description)
{
	for (size_t i = 0; i < s.length(); ++i) {
		char c = s[i];
		if (!iso9660_is_dchar(c)) {
			throw runtime_error((format("Illegal character '%1%' in %2% \"%3%\"") % c % description % s).str());
		}
	}
}


// Check that the given string only consists of a-characters.
static void checkAString(const string & s, const string & description)
{
	for (size_t i = 0; i < s.length(); ++i) {
		char c = s[i];
		if (!iso9660_is_achar(c)) {
			throw runtime_error((format("Illegal character '%1%' in %2% \"%3%\"") % c % description % s).str());
		}
	}
}


// Check that the given string is a valid file name.
static void checkFileName(const string & s, const string & description)
{
	for (size_t i = 0; i < s.length(); ++i) {
		char c = s[i];
		if (!iso9660_is_dchar(c) && c != '.') {
			throw runtime_error((format("Illegal character '%1%' in %2% \"%3%\"") % c % description % s).str());
		}
	}
}


// Check that the given string represents a valid sector number and
// convert it to an integer. Returns 0 if the string is empty;
static uint32_t checkLBN(const string & s, const string & itemName)
{
	uint32_t lbn = 0;
	if (!s.empty()) {
		try {
			lbn = boost::lexical_cast<uint32_t>(s);
			if (lbn <= ISO_EVD_SECTOR || lbn >= MAX_ISO_SECTORS) {
				throw runtime_error((format("Start LBN '%1%' of '%2%' is outside the valid range %3%..%4%") % s % itemName % ISO_EVD_SECTOR % MAX_ISO_SECTORS).str());
			}
		} catch (const boost::bad_lexical_cast &) {
			throw runtime_error((format("Invalid start LBN '%1%' specified for '%2%'") % s % itemName).str());
		}
	}
	return lbn;
}


// Parse the "system_area" section of the catalog file.
static void parseSystemArea(ifstream & catalogFile, Catalog & cat)
{
	while (true) {
		string line = nextline(catalogFile);
		if (line.empty()) {
			throw runtime_error("Syntax error in catalog file: unterminated system_area section");
		}

		static const boost::regex fileSpec("file\\s*\"(.+)\"");
		boost::smatch m;

		if (line == "}") {

			// End of section
			break;

		} else if (boost::regex_match(line, m, fileSpec)) {

			// File specification
			cat.systemAreaFile = m[1];

		} else {
			throw runtime_error((format("Syntax error in catalog file: \"%1%\" unrecognized in system_area section") % line).str());
		}
	}
}


// Parse the "volume" section of the catalog file.
static void parseVolume(ifstream & catalogFile, Catalog & cat)
{
	while (true) {
		string line = nextline(catalogFile);
		if (line.empty()) {
			throw runtime_error("Syntax error in catalog file: unterminated volume section");
		}

		static const boost::regex systemIdSpec("system_id\\s*\\[(.*)\\]");
		static const boost::regex volumeIdSpec("volume_id\\s*\\[(.*)\\]");
		static const boost::regex volumeSetIdSpec("volume_set_id\\s*\\[(.*)\\]");
		static const boost::regex publisherIdSpec("publisher_id\\s*\\[(.*)\\]");
		static const boost::regex preparerIdSpec("preparer_id\\s*\\[(.*)\\]");
		static const boost::regex applicationIdSpec("application_id\\s*\\[(.*)\\]");
		static const boost::regex copyrightFileIdSpec("copyright_file_id\\s*\\[(.*)\\]");
		static const boost::regex abstractFileIdSpec("abstract_file_id\\s*\\[(.*)\\]");
		static const boost::regex bibliographicFileIdSpec("bibliographic_file_id\\s*\\[(.*)\\]");
		static const boost::regex creationDateSpec("creation_date\\s*(.*)");
		static const boost::regex modificationDateSpec("modification_date\\s*(.*)");
		static const boost::regex expirationDateSpec("expiration_date\\s*(.*)");
		static const boost::regex effectiveDateSpec("effective_date\\s*(.*)");
		static const boost::regex defaultUIDSpec("default_uid\\s*(\\d+)");
		static const boost::regex defaultGIDSpec("default_gid\\s*(\\d+)");
		boost::smatch m;

		if (line == "}") {

			// End of section
			break;

		} else if (boost::regex_match(line, m, systemIdSpec)) {

			// System ID specification
			checkAString(m[1], "system_id");
			cat.systemID = m[1];

		} else if (boost::regex_match(line, m, volumeIdSpec)) {

			// Volume ID specification
			checkDString(m[1], "volume_id");
			cat.volumeID = m[1];

		} else if (boost::regex_match(line, m, volumeSetIdSpec)) {

			// Volume set ID specification
			checkDString(m[1], "volume_set_id");
			cat.volumeSetID = m[1];

		} else if (boost::regex_match(line, m, publisherIdSpec)) {

			// Publisher ID specification
			checkAString(m[1], "publisher_id");
			cat.publisherID = m[1];

		} else if (boost::regex_match(line, m, preparerIdSpec)) {

			// Preparer ID specification
			checkAString(m[1], "preparer_id");
			cat.preparerID = m[1];

		} else if (boost::regex_match(line, m, applicationIdSpec)) {

			// Application ID specification
			checkAString(m[1], "application_id");
			cat.applicationID = m[1];

		} else if (boost::regex_match(line, m, copyrightFileIdSpec)) {

			// Copyright file ID specification
			checkDString(m[1], "copyright_file_id");
			cat.copyrightFileID = m[1];

		} else if (boost::regex_match(line, m, abstractFileIdSpec)) {

			// Abstract file ID specification
			checkDString(m[1], "abstract_file_id");
			cat.abstractFileID = m[1];

		} else if (boost::regex_match(line, m, bibliographicFileIdSpec)) {

			// Bibliographic file ID specification
			checkDString(m[1], "bibliographic_file_id");
			cat.bibliographicFileID = m[1];

		} else if (boost::regex_match(line, m, creationDateSpec)) {

			// Creation date specification
			parse_ltime(m[1], cat.creationDate);

		} else if (boost::regex_match(line, m, modificationDateSpec)) {

			// Modification date specification
			parse_ltime(m[1], cat.modificationDate);

		} else if (boost::regex_match(line, m, expirationDateSpec)) {

			// Expiration date specification
			parse_ltime(m[1], cat.expirationDate);

		} else if (boost::regex_match(line, m, effectiveDateSpec)) {

			// Effective date specification
			parse_ltime(m[1], cat.effectiveDate);

		} else if (boost::regex_match(line, m, defaultUIDSpec)) {

			// Default user ID specification
			try {
				cat.defaultUID = boost::lexical_cast<uint16_t>(m[1]);
			} catch (boost::bad_lexical_cast &) {
				throw runtime_error((format("'%1%' is not a valid user ID") % m[1]).str());
			}

		} else if (boost::regex_match(line, m, defaultGIDSpec)) {

			// Default group ID specification
			try {
				cat.defaultGID = boost::lexical_cast<uint16_t>(m[1]);
			} catch (boost::bad_lexical_cast &) {
				throw runtime_error((format("'%1%' is not a valid group ID") % m[1]).str());
			}

		} else {
			throw runtime_error((format("Syntax error in catalog file: \"%1%\" unrecognized in volume section") % line).str());
		}
	}
}


// Recursively parse a "dir" section of the catalog file.
static DirNode * parseDir(ifstream & catalogFile, Catalog & cat, const string & dirName, const boost::filesystem::path & path, DirNode * parent = NULL, uint32_t startSector = 0)
{
	DirNode * dir = new DirNode(dirName, path, parent, startSector);

	while (true) {
		string line = nextline(catalogFile);
		if (line.empty()) {
			throw runtime_error((format("Syntax error in catalog file: unterminated directory section \"%1%\"") % dirName).str());
		}

		static const boost::regex fileSpec("file\\s*(\\S+)(?:\\s*@(\\d+))?");
		static const boost::regex xaFileSpec("xafile\\s*(\\S+)(?:\\s*@(\\d+))?");
		static const boost::regex dirStart("dir\\s*(\\S+)(?:\\s*@(\\d+))?\\s*\\{");
		boost::smatch m;

		if (line == "}") {

			// End of section
			break;

		} else if (boost::regex_match(line, m, fileSpec)) {

			// File specification
			string fileName = m[1];
			checkFileName(fileName, "file name");

			uint32_t startSector = checkLBN(m[2], fileName);

			FileNode * file = new FileNode(fileName + ";1", path / fileName, dir, startSector);
			dir->children.push_back(file);

		} else if (boost::regex_match(line, m, xaFileSpec)) {

			// XA file specification
			string fileName = m[1];
			checkFileName(fileName, "file name");

			uint32_t startSector = checkLBN(m[2], fileName);

			FileNode * file = new FileNode(fileName + ";1", path / fileName, dir, startSector, true);
			dir->children.push_back(file);

		} else if (boost::regex_match(line, m, dirStart)) {

			// Subdirectory section
			string subDirName = m[1];
			checkDString(subDirName, "directory name");

			uint32_t startSector = checkLBN(m[2], subDirName);

			DirNode * subDir = parseDir(catalogFile, cat, subDirName, path / subDirName, dir, startSector);
			dir->children.push_back(subDir);

		} else {
			throw runtime_error((format("Syntax error in catalog file: \"%1%\" unrecognized in directory section") % line).str());
		}
	}

	// Create the sorted list of children
	dir->sortedChildren = dir->children;
	sort(dir->sortedChildren.begin(), dir->sortedChildren.end(), CmpByName());

	return dir;
}


// Parse the catalog file and fill in the Catalog structure.
static void parseCatalog(ifstream & catalogFile, Catalog & cat, const boost::filesystem::path & fsBase)
{
	while (true) {
		string line = nextline(catalogFile);
		if (line.empty()) {

			// End of file
			return;
		}

		static const boost::regex systemAreaStart("system_area\\s*\\{");
		static const boost::regex volumeStart("volume\\s*\\{");
		static const boost::regex rootDirStart("dir\\s*\\{");

		if (boost::regex_match(line, systemAreaStart)) {

			// Parse system_area section
			parseSystemArea(catalogFile, cat);

		} else if (boost::regex_match(line, volumeStart)) {

			// Parse volume section
			parseVolume(catalogFile, cat);

		} else if (boost::regex_match(line, rootDirStart)) {

			// Parse root directory entry
			if (cat.root) {
				throw runtime_error("More than one root directory section in catalog file");
			} else {
				cat.root = parseDir(catalogFile, cat, "", fsBase);
			}

		} else {
			throw runtime_error((format("Syntax error in catalog file: \"%1%\" unrecognized") % line).str());
		}
	}
}


// Visitor which prints the filesystem tree to cout
class PrintVisitor : public Visitor {
public:
	void visit(FileNode & file)
	{
		cout << file.path << " (" << file.numSectors << " sectors @ " << file.firstSector << ", " << file.size << " bytes)" << endl;
	}

	void visit(DirNode & dir)
	{
		cout << dir.path << " (" << dir.numSectors << " sectors @ " << dir.firstSector << ", PT record " << dir.recordNumber << ")" << endl;
	}
};


// Visitor which calculates the number of sectors required for each directory,
// setting the "numSectors" field of all directory nodes
class CalcDirSize : public Visitor {
public:
	void visit(DirNode & dir)
	{
		uint32_t size = 0;

		// "." and ".." records
		size += iso9660_dir_calc_record_size(1, sizeof(iso9660_xa_t));
		size += iso9660_dir_calc_record_size(1, sizeof(iso9660_xa_t));

		// Records for all direct children
		for (vector<FSNode *>::const_iterator i = dir.sortedChildren.begin(); i != dir.sortedChildren.end(); ++i) {
			uint32_t recordSize = iso9660_dir_calc_record_size((*i)->name.size(), sizeof(iso9660_xa_t));

			if (size / ISO_BLOCKSIZE != (size + recordSize) / ISO_BLOCKSIZE) {

				// Record would cross a sector boundary, add padding
				recordSize += (ISO_BLOCKSIZE - size) % ISO_BLOCKSIZE;
			}

			size += recordSize;
		}

		// Round up to full sectors
		dir.numSectors = (size + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE;
	}
};


// Visitor which allocates sectors to all file and directory extents, setting
// the "firstSector" field of all nodes
class AllocSectors : public Visitor {
public:
	AllocSectors(uint32_t startSector_) : currentSector(startSector_) { }

	uint32_t getCurrentSector() const { return currentSector; }

	void visitNode(FSNode & node)
	{
		// Minimum start sector requested?
		if (node.requestedStartSector) {

			// Yes, before current sector?
			if (node.requestedStartSector < currentSector) {

				// Yes, ignore the request and print a warning
				node.firstSector = currentSector;
				cerr << "Warning: " << node.path << " will start at sector " << node.firstSector << " instead of " << node.requestedStartSector << endl;

			} else {

				// Heed the request
				node.firstSector = node.requestedStartSector;
			}

		} else {

			// Allocate contiguously
			node.firstSector = currentSector;
		}

		currentSector = node.firstSector + node.numSectors;
	}

	void visit(DirNode & dir) { visitNode(dir); }
	void visit(FileNode & file) { visitNode(file); }

private:
	uint32_t currentSector;
};


// Visitor which creates the directory data, setting the "data" field of
// directory nodes
class MakeDirectories : public Visitor {
public:
	MakeDirectories(const Catalog & cat_) : cat(cat_) { }

	void visit(DirNode & dir)
	{
		uint32_t dirSize = dir.numSectors * ISO_BLOCKSIZE;

		// Create the directory extent
		iso9660_xa_t xaAttr;
		iso9660_xa_init(&xaAttr, 0, 0, XA_FORM1_DIR, 0);

		uint32_t parentSector = dir.parent ? dir.parent->firstSector : dir.firstSector;
		uint32_t parentSize = (dir.parent ? dir.parent->numSectors : dir.numSectors) * ISO_BLOCKSIZE;

		struct tm dirTm;
		iso9660_get_ltime(&cat.creationDate, &dirTm);
		time_t dirTime = mktime(&dirTm);

		uint8_t * data = new uint8_t[dirSize];
		iso9660_dir_init_new_su(data,
		                        dir.firstSector, dirSize, &xaAttr, sizeof(xaAttr),
		                        parentSector, parentSize, &xaAttr, sizeof(xaAttr),
		                        &dirTime);

		// Add the records for all children
		for (vector<FSNode *>::const_iterator i = dir.sortedChildren.begin(); i != dir.sortedChildren.end(); ++i) {
			FSNode * node = *i;
			uint32_t size = node->numSectors * ISO_BLOCKSIZE;
			uint8_t flags = ISO_FILE | ISO_EXISTENCE;

			if (FileNode * file = dynamic_cast<FileNode *>(node)) {
				if (file->isForm2) {
					iso9660_xa_init(&xaAttr, cat.defaultUID, cat.defaultGID, XA_FORM2_FILE, 1);
				} else {
					iso9660_xa_init(&xaAttr, cat.defaultUID, cat.defaultGID, XA_FORM1_FILE, 0);
					size = file->size;
				}
			} else if (DirNode * dir = dynamic_cast<DirNode *>(node)) {
				iso9660_xa_init(&xaAttr, 0, 0, XA_FORM1_DIR, 0);
				flags = ISO_DIRECTORY | ISO_EXISTENCE;
			} else {
				throw runtime_error("Internal filesystem tree corrupt");
			}

			iso9660_dir_add_entry_su(data, node->name.c_str(), node->firstSector, size, flags, &xaAttr, sizeof(xaAttr), &dirTime);
		}

		dir.data = data;
	}

private:

	// Reference to Catalog information
	const Catalog & cat;
};


// Visitor which constructs the path tables from a filesystem tree,
// assigning the "recordNumber" field of all directory nodes
class PathTables : public Visitor {
public:
	PathTables()
	{
		iso9660_pathtable_init(lTable);
		iso9660_pathtable_init(mTable);
	}

	void visit(DirNode & dir)
	{
		uint16_t parentRecord = dir.parent ? dir.parent->recordNumber : 1;
		dir.recordNumber = iso9660_pathtable_l_add_entry(lTable, dir.name.c_str(), dir.firstSector, parentRecord);
		dir.recordNumber = iso9660_pathtable_m_add_entry(mTable, dir.name.c_str(), dir.firstSector, parentRecord);
	}

	size_t size() const { return iso9660_pathtable_get_size( lTable ); }
	const void * getLTable() const { return lTable; }
	const void * getMTable() const { return mTable; }

private:
	uint8_t lTable[ISO_BLOCKSIZE];  // LSB-first table
	uint8_t mTable[ISO_BLOCKSIZE];  // MSB-first table
};


// Visitor which writes all directory and file data to the image file
class WriteData : public Visitor {
public:
	WriteData(ofstream & image_, uint32_t startSector_) : image(image_), currentSector(startSector_) { }

	void visit(FileNode & file)
	{
		ifstream f(file.path.c_str(), ifstream::in | ifstream::binary);
		if (!f) {
			throw runtime_error((format("Cannot open file %1%") % file.path).str());
		}

		cdio_info("Writing \"%s\"...", file.path.c_str());

		writeGap(file.firstSector);

		char data[M2RAW_SECTOR_SIZE];
		size_t blockSize = file.isForm2 ? M2RAW_SECTOR_SIZE : ISO_BLOCKSIZE;

		for (uint32_t sector = 0; sector < file.numSectors; ++sector) {
			uint8_t subMode = SM_DATA;
			if (sector == file.numSectors - 1) {
				subMode |= (SM_EOF | SM_EOR);  // last sector
			}

			memset(data, 0, blockSize);
			f.read(data, blockSize);

			if (file.isForm2) {
				_vcd_make_mode2(buffer, data + CDIO_CD_SUBHEADER_SIZE, currentSector, data[0], data[1], data[2], data[3]);
			} else {
				_vcd_make_mode2(buffer, data, currentSector, 0, 0, subMode, 0);
			}

			image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

			++currentSector;
		}
	}

	void visit(DirNode & dir)
	{
		writeGap(dir.firstSector);

		for (uint32_t sector = 0; sector < dir.numSectors; ++sector) {
			uint8_t subMode = SM_DATA;
			if (sector == dir.numSectors - 1) {
				subMode |= (SM_EOF | SM_EOR);  // last sector
			}

			_vcd_make_mode2(buffer, dir.data + sector * ISO_BLOCKSIZE, currentSector, 0, 0, subMode, 0);
			image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

			++currentSector;
		}
	}

	// Write empty sectors as a gap until we reach the specified sector.
	void writeGap(uint32_t until)
	{
		while (currentSector < until) {
			_vcd_make_mode2(buffer, emptySector, currentSector, 0, 0, SM_FORM2, 0);
			image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

			++currentSector;
		}
	}

private:
	ofstream & image;
	uint32_t currentSector;
};


// Write the system area to the image file, optionally using the file
// specified in the catalog as input.
static void writeSystemArea(ofstream & image, const Catalog & cat)
{
	const size_t numSystemSectors = 16;
	const size_t systemAreaSize = numSystemSectors * CDIO_CD_FRAMESIZE;

	boost::scoped_array<char> data(new char[systemAreaSize]);
	memset(data.get(), 0, systemAreaSize);

	size_t fileSize = 0;

	if (!cat.systemAreaFile.empty()) {

		// Copy the data (max. 32K) from the system area file
		ifstream f(cat.systemAreaFile.c_str(), ifstream::in | ifstream::binary);
		if (!f) {
			throw runtime_error((format("Cannot open system area file \"%1%\"") % cat.systemAreaFile).str());
		}

		fileSize = f.read(data.get(), systemAreaSize).gcount();
		if (f.bad()) {
			throw runtime_error((format("Error reading system area file \"%1%\"") % cat.systemAreaFile).str());
		}
	}

	size_t numFileSectors = (fileSize + CDIO_CD_FRAMESIZE - 1) / CDIO_CD_FRAMESIZE;

	// Write system area to image file
	for (size_t sector = 0; sector < numFileSectors; ++sector) {

		// Data sectors
		_vcd_make_mode2(buffer, data.get() + sector * CDIO_CD_FRAMESIZE, sector, 0, 0, SM_DATA, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);
	}

	for (size_t sector = numFileSectors; sector < numSystemSectors; ++sector) {

		// Empty sectors
		_vcd_make_mode2(buffer, emptySector, sector, 0, 0, SM_FORM2, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);
	}
}


// Print usage information and exit.
static void usage(const char * progname, int exitcode = 0, const string & error = "")
{
	cout << "Usage: " << boost::filesystem::path(progname).filename().native() << " [OPTION...] <input>[.cat] [<output>[.bin]]" << endl;
    cout << "  -c, --cuefile                   Create a .cue file" << endl;
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
	boost::filesystem::path inputPath;
	boost::filesystem::path outputPath;
	bool verbose = false;
	bool writeCueFile = false;

	for (int i = 1; i < argc; ++i) {
		string arg = argv[i];

		if (arg == "--version" || arg == "-V") {
			cout << TOOL_VERSION << endl;
			return 0;
		} else if (arg == "--cuefile" || arg == "-c") {
			writeCueFile = true;
		} else if (arg == "--verbose" || arg == "-v") {
			cdio_loglevel_default = CDIO_LOG_INFO;
			verbose = true;
		} else if (arg == "--help" || arg == "-?") {
			usage(argv[0]);
		} else if (arg[0] == '-') {
			usage(argv[0], 64, "Invalid option '" + arg + "'");
		} else {
			if (inputPath.empty()) {
				inputPath = arg;
			} else if (outputPath.empty()) {
				outputPath = arg;
			} else {
				usage(argv[0], 64, "Unexpected extra argument '" + arg + "'");
			}
		}
	}

	if (inputPath.empty()) {
		usage(argv[0], 64, "No input catalog file specified");
	}

	if (outputPath.empty()) {
		outputPath = inputPath;
		outputPath.replace_extension("");
	}

	try {

		// Read and parse the catalog file
		boost::filesystem::path catalogName = inputPath;
		if (catalogName.extension().empty()) {
			catalogName.replace_extension(".cat");
		}

		Catalog cat;

		ifstream catalogFile(catalogName.c_str());
		if (!catalogFile) {
			throw runtime_error((format("Cannot open catalog file %1%") % catalogName).str());
		}

		boost::filesystem::path fsBasePath = inputPath;
		fsBasePath.replace_extension("");

		cout << "Reading catalog file " << catalogName << "...\n";
		cout << "Reading filesystem from directory " << fsBasePath << "...\n";

		parseCatalog(catalogFile, cat, fsBasePath);

		if (!cat.root) {
			throw runtime_error("No root directory specified in catalog file");
		}

		catalogFile.close();

		// Calculate the sector numbers of the fixed data structures
		const uint32_t pvdSector = ISO_PVD_SECTOR;
		const uint32_t evdSector = pvdSector + 1;
		const uint32_t pathTableStartSector = evdSector + 1;
		const uint32_t numPathTableSectors = 1;  // number of sectors in one path table, currently fixed to 1
		const uint32_t rootDirStartSector = pathTableStartSector + numPathTableSectors * 4;  // 2 types and 2 copies in path table group

		// Calculate the sizes of all directories
		CalcDirSize calcDir;
		cat.root->traverseSorted(calcDir);

		// Allocate start sectors to all nodes
		AllocSectors alloc(rootDirStartSector);
		cat.root->traverse(alloc);  // must use the same traversal order as "WriteData" below

		uint32_t volumeSize = alloc.getCurrentSector();
		if (volumeSize > MAX_ISO_SECTORS) {
			cerr << "Warning: Output image larger than "
			     << (MAX_ISO_SECTORS * CDIO_CD_FRAMESIZE_RAW / (1024*1024)) << " MiB\n";
		}

		// Create the directory data
		MakeDirectories makeDirs(cat);
		cat.root->traverseSorted(makeDirs);

		// Create the path tables
		PathTables pathTables;
		cat.root->traverseBreadthFirstSorted(pathTables);

		if (pathTables.size() > ISO_BLOCKSIZE) {
			throw runtime_error("The path table is larger than one sector. This is currently not supported.");
		}

		if (verbose) {
			PrintVisitor pv;
			cat.root->traverse(pv);
		}

		// Create the image file
		boost::filesystem::path imageName = outputPath;
		imageName.replace_extension(".bin");

		ofstream image(imageName.c_str(), ofstream::out | ofstream::binary | ofstream::trunc);
		if (!image) {
			throw runtime_error((format("Error creating image file %1%") % imageName).str());
		}

		// Write the system area
		cdio_info("Writing system area...");
		writeSystemArea(image, cat);

		// Write the PVD
		cdio_info("Writing volume descriptors...");
		iso9660_pvd_t volumeDesc;

		struct tm rootTm;
		iso9660_get_ltime(&cat.creationDate, &rootTm);
		time_t rootTime = mktime(&rootTm);

		iso9660_dir_t rootDirRecord;
		memset(&rootDirRecord, 0, sizeof(rootDirRecord));

		rootDirRecord.length = to_711(iso9660_dir_calc_record_size(0, 0));
		rootDirRecord.extent = to_733(rootDirStartSector);
		rootDirRecord.size = to_733(cat.root->numSectors * ISO_BLOCKSIZE);
		iso9660_set_dtime(&rootTm, &rootDirRecord.recording_time);
		rootDirRecord.file_flags = ISO_DIRECTORY;
		rootDirRecord.volume_sequence_number = to_723(1);
		rootDirRecord.filename.len = 1;

		iso9660_set_pvd(&volumeDesc,
		                cat.volumeID.c_str(), cat.publisherID.c_str(), cat.preparerID.c_str(), cat.applicationID.c_str(),
		                volumeSize, &rootDirRecord,
		                pathTableStartSector, pathTableStartSector + numPathTableSectors * 2,
		                pathTables.size(), &rootTime);

		iso9660_strncpy_pad(volumeDesc.system_id, cat.systemID.c_str(), ISO_MAX_SYSTEM_ID, ISO9660_ACHARS);
		iso9660_strncpy_pad(volumeDesc.volume_set_id, cat.volumeSetID.c_str(), ISO_MAX_VOLUMESET_ID, ISO9660_DCHARS);
		iso9660_strncpy_pad(volumeDesc.copyright_file_id, cat.copyrightFileID.c_str(), MAX_ISONAME, ISO9660_DCHARS);
		iso9660_strncpy_pad(volumeDesc.abstract_file_id, cat.abstractFileID.c_str(), MAX_ISONAME, ISO9660_DCHARS);
		iso9660_strncpy_pad(volumeDesc.bibliographic_file_id, cat.bibliographicFileID.c_str(), MAX_ISONAME, ISO9660_DCHARS);

		volumeDesc.creation_date = cat.creationDate;
		volumeDesc.modification_date = cat.modificationDate;
		volumeDesc.expiration_date = cat.expirationDate;
		volumeDesc.effective_date = cat.effectiveDate;

		volumeDesc.opt_type_l_path_table = to_731(pathTableStartSector + numPathTableSectors);
		volumeDesc.opt_type_m_path_table = to_732(pathTableStartSector + numPathTableSectors * 3);

		_vcd_make_mode2(buffer, &volumeDesc, pvdSector, 0, 0, SM_DATA | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		// Write the volume descriptor set terminator
		iso9660_set_evd(&volumeDesc);

		_vcd_make_mode2(buffer, &volumeDesc, evdSector, 0, 0, SM_DATA | SM_EOF | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		// Write the path tables
		cdio_info("Writing path tables...");
		_vcd_make_mode2(buffer, pathTables.getLTable(), pathTableStartSector + numPathTableSectors * 0, 0, 0, SM_DATA | SM_EOF | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		_vcd_make_mode2(buffer, pathTables.getLTable(), pathTableStartSector + numPathTableSectors * 1, 0, 0, SM_DATA | SM_EOF | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		_vcd_make_mode2(buffer, pathTables.getMTable(), pathTableStartSector + numPathTableSectors * 2, 0, 0, SM_DATA | SM_EOF | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		_vcd_make_mode2(buffer, pathTables.getMTable(), pathTableStartSector + numPathTableSectors * 3, 0, 0, SM_DATA | SM_EOF | SM_EOR, 0);
		image.write(buffer, CDIO_CD_FRAMESIZE_RAW);

		// Write the directory and file data
		WriteData writeData(image, rootDirStartSector);
		cat.root->traverse(writeData);  // must use the same traversal order as "AllocSectors" above

		// Close the image file
		if (!image) {
			throw runtime_error((format("Error writing to image file %1%") % imageName).str());
		}
		image.close();

		cout << "Image file written to " << imageName << endl;

		// Write the cue file
		if (writeCueFile) {
			boost::filesystem::path cueName = outputPath;
			cueName.replace_extension(".cue");

			ofstream cueFile(cueName.c_str(), ofstream::out | ofstream::trunc);
			if (!cueFile) {
				throw runtime_error((format("Error creating cue file %1%") % cueName).str());
			}

			cueFile << "FILE " << imageName << " BINARY\r\n";
			cueFile << "  TRACK 01 MODE2/2352\r\n";
			cueFile << "    INDEX 01 00:00:00\r\n";

			if (!cueFile) {
				throw runtime_error((format("Error writing to cue file %1%") % cueName).str());
			}
			cueFile.close();

			cout << "Cue file written to " << cueName << endl;
		}

		cdio_info("Done.");

	} catch (const std::exception & e) {
		cerr << e.what() << endl;
		return 1;
	}

	return 0;
}
