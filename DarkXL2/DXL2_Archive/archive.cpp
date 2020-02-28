#include "archive.h"
#include "gobArchive.h"
#include "lfdArchive.h"
#include <assert.h>
#include <string>
#include <map>

namespace
{
	typedef std::map<std::string, Archive*> ArchiveMap;
	static ArchiveMap s_archives[ARCHIVE_COUNT];
}

Archive* Archive::getArchive(ArchiveType type, const char* name, const char* path)
{
	ArchiveMap::iterator iArchive = s_archives[type].find(name);
	if (iArchive != s_archives[type].end())
	{
		return iArchive->second;
	}

	Archive* archive = nullptr;
	switch (type)
	{
		case ARCHIVE_GOB:
		{
			archive = new GobArchive();
		}
		break;
		case ARCHIVE_LFD:
		{
			archive = new LfdArchive();
		}
		break;
		default:
			assert(0);
		break;
	};

	if (archive)
	{
		strcpy(archive->m_name, name);
		archive->open(path);
		archive->m_type = type;
		(s_archives[type])[name] = archive;
	}
	return archive;
}

void Archive::freeArchive(Archive* archive)
{
	if (!archive) { return; }

	const ArchiveType type = archive->m_type;
	ArchiveMap::iterator iArchive = s_archives[type].find(archive->m_name);
	if (iArchive != s_archives[archive->m_type].end())
	{
		s_archives[type].erase(iArchive);
	}

	archive->close();
	delete archive;
}

void Archive::freeAllArchives()
{
	for (u32 i = 0; i < ARCHIVE_COUNT; i++)
	{
		ArchiveMap::iterator iArchive = s_archives[i].begin();
		for (; iArchive != s_archives[i].end(); ++iArchive)
		{
			Archive* archive = iArchive->second;
			archive->close();
			delete archive;
		}
		s_archives[i].clear();
	}
}