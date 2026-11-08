/* ***** BEGIN LICENSE BLOCK *****
*   Copyright (C) 2012-2016, Peter Hatina <phatina@gmail.com>
*
*   This program is free software; you can redistribute it and/or
*   modify it under the terms of the GNU General Public License as
*   published by the Free Software Foundation; either version 2 of
*   the License, or (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program. If not, see <http://www.gnu.org/licenses/>.
* ***** END LICENSE BLOCK ***** */

#include <config.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
extern "C" {
#  include <libmtp.h>
}
#include "simple-mtpfs-type-dir.h"

TypeDir::TypeDir():
    TypeBasic(),
    m_dirs(),
    m_files(),
    m_access_mutex(),
    m_fetched(false),
    m_modif_date(0)
{
}

TypeDir::TypeDir(uint32_t id, uint32_t parent_id, uint32_t storage_id,
    const std::string &name):
    TypeBasic(id, parent_id, storage_id, name),
    m_dirs(),
    m_files(),
    m_access_mutex(),
    m_fetched(false),
    m_modif_date(0)
{
}

TypeDir::TypeDir(LIBMTP_file_t *file):
    TypeBasic(file->item_id, file->parent_id,
        file->storage_id, std::string(file->filename)),
    m_dirs(),
    m_files(),
    m_access_mutex(),
    m_fetched(false),
    m_modif_date(file->modificationdate)
{
}

TypeDir::TypeDir(const TypeDir &copy):
    TypeBasic(copy),
    m_dirs(copy.m_dirs),
    m_files(copy.m_files),
    m_access_mutex(),
    m_fetched(copy.m_fetched.load()),
    m_modif_date(copy.m_modif_date)
{
}

LIBMTP_folder_t *TypeDir::toLIBMTPFolder() const
{
    LIBMTP_folder_t *f = static_cast<LIBMTP_folder_t*>(
        malloc(sizeof(LIBMTP_folder_t)));
    if (!f)
        return nullptr;
    f->folder_id = m_id;
    f->parent_id = m_parent_id;
    f->storage_id = m_storage_id;
    f->name = strdup(m_name.c_str());
    f->sibling = nullptr;
    f->child = nullptr;
    return f;
}

void TypeDir::addDir(const TypeDir &dir)
{
    enterCritical();
    m_dirs.insert(dir);
    leaveCritical();
}

void TypeDir::addFile(const TypeFile &file)
{
    enterCritical();
    m_files.insert(file);
    leaveCritical();
}

bool TypeDir::removeDir(const TypeDir &dir)
{
    std::lock_guard<std::mutex> l(m_access_mutex);
    auto it = std::find(m_dirs.begin(), m_dirs.end(), dir);
    if (it == m_dirs.end() || it->isDeleted())
        return false;
    it->markDeleted();
    return true;
}

bool TypeDir::removeFile(const TypeFile &file)
{
    std::lock_guard<std::mutex> l(m_access_mutex);
    auto it = std::find(m_files.begin(), m_files.end(), file);
    if (it == m_files.end() || it->isDeleted())
        return false;
    it->markDeleted();
    return true;
}

bool TypeDir::replaceFile(const TypeFile &oldfile, const TypeFile &newfile)
{
    std::lock_guard<std::mutex> l(m_access_mutex);
    auto it = std::find(m_files.begin(), m_files.end(), oldfile);
    if (it != m_files.end())
        it->markDeleted();
    m_files.insert(newfile);
    return it != m_files.end();
}

bool TypeDir::renameFile(const TypeFile &file, const std::string &new_name)
{
    std::lock_guard<std::mutex> l(m_access_mutex);
    auto it = std::find(m_files.begin(), m_files.end(), file);
    if (it == m_files.end() || it->isDeleted())
        return false;
    TypeFile renamed(*it);
    renamed.setName(new_name);
    it->markDeleted();
    m_files.insert(renamed);
    return true;
}

bool TypeDir::renameDir(const TypeDir &dir, const std::string &new_name)
{
    std::lock_guard<std::mutex> l(m_access_mutex);
    auto it = std::find(m_dirs.begin(), m_dirs.end(), dir);
    if (it == m_dirs.end() || it->isDeleted())
        return false;
    // Fresh, unfetched node: its content is listed again from the device on demand,
    // instead of deep-copying a subtree other threads may be using.
    TypeDir renamed(it->id(), it->parentid(), it->storageid(), new_name);
    renamed.setModificationDate(it->modificationDate());
    it->markDeleted();
    m_dirs.insert(renamed);
    return true;
}

TypeDir &TypeDir::operator =(const TypeDir &rhs)
{
    TypeBasic::operator =(rhs);
    m_dirs = rhs.m_dirs;
    m_files = rhs.m_files;
    m_fetched = rhs.m_fetched.load();
    return *this;
}

const TypeDir *TypeDir::dir(const std::string &name) const
{
    std::lock_guard<std::mutex> l(m_access_mutex);
    for (const TypeDir &d : m_dirs)
        if (!d.isDeleted() && d == name)
            return &d;
    return nullptr;
}

const TypeFile *TypeDir::file(const std::string &name) const
{
    std::lock_guard<std::mutex> l(m_access_mutex);
    for (const TypeFile &f : m_files)
        if (!f.isDeleted() && f == name)
            return &f;
    return nullptr;
}

std::vector<TypeDir::Entry> TypeDir::entries() const
{
    std::lock_guard<std::mutex> l(m_access_mutex);
    std::vector<Entry> out;
    out.reserve(m_dirs.size() + m_files.size());
    for (const TypeDir &d : m_dirs)
        if (!d.isDeleted())
            out.push_back(Entry{d.name(), d.id(), true, 0, d.modificationDate()});
    for (const TypeFile &f : m_files)
        if (!f.isDeleted())
            out.push_back(Entry{f.name(), f.id(), false, f.size(), f.modificationDate()});
    return out;
}

std::string TypeDir::firstDirName() const
{
    std::lock_guard<std::mutex> l(m_access_mutex);
    for (const TypeDir &d : m_dirs)
        if (!d.isDeleted())
            return d.name();
    return std::string();
}

std::set<TypeDir>::size_type TypeDir::dirCount() const
{
    std::lock_guard<std::mutex> l(m_access_mutex);
    std::set<TypeDir>::size_type n = 0;
    for (const TypeDir &d : m_dirs)
        n += !d.isDeleted();
    return n;
}

std::set<TypeFile>::size_type TypeDir::fileCount() const
{
    std::lock_guard<std::mutex> l(m_access_mutex);
    std::set<TypeFile>::size_type n = 0;
    for (const TypeFile &f : m_files)
        n += !f.isDeleted();
    return n;
}

bool TypeDir::isEmpty() const
{
    return dirCount() == 0 && fileCount() == 0;
}
