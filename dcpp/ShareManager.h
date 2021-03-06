/*
 * Copyright (C) 2001-2012 Jacek Sieka, arnetheduck on gmail point com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "NonCopyable.h"

#include "TimerManager.h"
#include "SearchManager.h"
#include "SettingsManager.h"
#include "HashManagerListener.h"
#include "QueueManagerListener.h"

#include "Exception.h"
#include "CriticalSection.h"

#include "StringSearch.h"
#include "Singleton.h"
#include "BloomFilter.h"
#include "FastAlloc.h"
#include "MerkleTree.h"
#include "Pointer.h"
#include "Atomic.h"

#ifdef WITH_DHT
namespace dht {
class IndexManager;
}
#endif

namespace dcpp {

using std::function;
using std::map;
using std::set;
using std::unique_ptr;
using std::unordered_map;

STANDARD_EXCEPTION(ShareException);

class SimpleXML;
class Client;
class File;
class OutputStream;
class MemoryInputStream;

struct ShareLoader;
class ShareManager : public Singleton<ShareManager>, private SettingsManagerListener, private Thread, private TimerManagerListener,
        private HashManagerListener, private QueueManagerListener
{
public:
    /**
     * @param aDirectory Physical directory location
     * @param aName Virtual name
     */
    void addDirectory(const string& realPath, const string &virtualName);
    void removeDirectory(const string& realPath);
    void renameDirectory(const string& realPath, const string& virtualName);

    bool isRefreshing() { return refreshing; }

    string toVirtual(const TTHValue& tth) const;
    string toReal(const string& virtualFile);
    StringList getRealPaths(const string& virtualPath);
    TTHValue getTTH(const string& virtualFile) const;

    void refresh(bool dirs = false, bool aUpdate = true, bool block = false) noexcept;
    void setDirty() { xmlDirty = true; }

    void search(SearchResultList& l, const string& aString, int aSearchType, int64_t aSize, int aFileType, Client* aClient, StringList::size_type maxResults) noexcept;
    void search(SearchResultList& l, const StringList& params, StringList::size_type maxResults) noexcept;

    StringPairList getDirectories() const noexcept;

    MemoryInputStream* generatePartialList(const string& dir, bool recurse) const;
    MemoryInputStream* getTree(const string& virtualFile) const;

    AdcCommand getFileInfo(const string& aFile);

    int64_t getShareSize() const noexcept;
    int64_t getShareSize(const string& realPath) const noexcept;

    size_t getSharedFiles() const noexcept;

    string getShareSizeString() const { return Util::toString(getShareSize()); }
    string getShareSizeString(const string& aDir) const { return Util::toString(getShareSize(aDir)); }

    void getBloom(ByteVector& v, size_t k, size_t m, size_t h) const;

    SearchManager::TypeModes getType(const string& fileName) const noexcept;

    string validateVirtual(const string& /*aVirt*/) const noexcept;
    bool hasVirtual(const string& name) const noexcept;

    void addHits(uint32_t aHits) {
        hits += aHits;
    }

    const string getOwnListFile() {
        generateXmlList();
        return getBZXmlFile();
    }

    bool isTTHShared(const TTHValue& tth){
        Lock l(cs);
        return tthIndex.find(tth) != tthIndex.end();
    }
    void publish();

    GETSET(uint32_t, hits, Hits);
    GETSET(string, bzXmlFile, BZXmlFile);

private:
    struct AdcSearch;
    class Directory : public FastAlloc<Directory>, public intrusive_ptr_base<Directory>, private NonCopyable {
    public:
        typedef boost::intrusive_ptr<Directory> Ptr;
        typedef unordered_map<string, Ptr, CaseStringHash, CaseStringEq> Map;
        typedef Map::iterator MapIter;

        struct File {
            File() : size(0), parent(0) { }
            File(const string& aName, int64_t aSize, const Directory::Ptr& aParent, const TTHValue& aRoot) :
                name(aName), tth(aRoot), size(aSize), parent(aParent.get()) { }
            File(const File& rhs) :
                name(rhs.getName()), tth(rhs.getTTH()), size(rhs.getSize()), parent(rhs.getParent()) { }

            File& operator=(const File& rhs) {
                name = rhs.name; size = rhs.size; parent = rhs.parent; tth = rhs.tth;
                return *this;
            }

            bool operator==(const File& rhs) const {
                if (BOOLSETTING(CASESENSITIVE_FILELIST))
                    return getParent() == rhs.getParent() && (strcmp(getName().c_str(), rhs.getName().c_str()) == 0);
                else
                    return getParent() == rhs.getParent() && (Util::stricmp(getName(), rhs.getName()) == 0);
            }

            struct StringComp {
                StringComp(const string& s) : a(s) { }
                bool operator()(const File& b) const {
                    if (BOOLSETTING(CASESENSITIVE_FILELIST))
                        return strcmp(a.c_str(), b.getName().c_str()) == 0;
                    else
                        return Util::stricmp(a, b.getName()) == 0;
                }

                const string& a;

            private:
                StringComp& operator=(const StringComp&);
            };

            struct FileLess {
                bool operator()(const File& a, const File& b) const {
                    if (BOOLSETTING(CASESENSITIVE_FILELIST))
                        return (strcmp(a.getName().c_str(), b.getName().c_str()) < 0);
                    else
                        return (Util::stricmp(a.getName(), b.getName()) < 0);
                }
            };

            typedef set<File, FileLess> Set;

            string getADCPath() const { return parent->getADCPath() + name; }
            string getFullName() const { return parent->getFullName() + name; }
            string getRealPath() const { return parent->getRealPath(name); }

            GETSET(string, name, Name);
            GETSET(TTHValue, tth, TTH);
            GETSET(int64_t, size, Size);
            GETSET(Directory*, parent, Parent);
        };

        int64_t size;
        Map directories;
        set<File, File::FileLess> files;

        static Ptr create(const string& aName, const Ptr& aParent = Ptr()) { return Ptr(new Directory(aName, aParent)); }

        bool hasType(uint32_t type) const noexcept {
            return ( (type == SearchManager::TYPE_ANY) || (fileTypes & (1 << type)) );
        }
        void addType(uint32_t type) noexcept;

        string getADCPath() const noexcept;
        string getFullName() const noexcept;
        string getRealPath(const std::string& path) const;

        int64_t getSize() const noexcept;

        void search(SearchResultList& aResults, StringSearch::List& aStrings, int aSearchType, int64_t aSize, int aFileType, Client* aClient, StringList::size_type maxResults) const noexcept;
        void search(SearchResultList& aResults, AdcSearch& aStrings, StringList::size_type maxResults) const noexcept;

        void toXml(OutputStream& xmlFile, string& indent, string& tmp2, bool fullList) const;
        void filesToXml(OutputStream& xmlFile, string& indent, string& tmp2) const;

        auto findFile(const string& aFile) const -> decltype(files.cbegin()) { return find_if(files.begin(), files.end(), Directory::File::StringComp(aFile)); }

        void merge(const Ptr& source);

        GETSET(string, name, Name);
        GETSET(Directory*, parent, Parent);
    private:
        friend void intrusive_ptr_release(intrusive_ptr_base<Directory>*);

        Directory(const string& aName, const Ptr& aParent);
        ~Directory() { }

        /** Set of flags that say which SearchManager::TYPE_* a directory contains */
        uint32_t fileTypes;

    };

    friend class Directory;
    friend struct ShareLoader;

    friend class Singleton<ShareManager>;
    ShareManager();

    virtual ~ShareManager();

    struct AdcSearch {
        AdcSearch(const StringList& adcParams);

        bool isExcluded(const string& str);
        bool hasExt(const string& name);
        StringSearch::List* include;
        StringSearch::List includeInit;
        StringSearch::List exclude;
        StringList ext;
        StringList noExt;

        int64_t gt;
        int64_t lt;

        TTHValue root;
        bool hasRoot;

        bool isDirectory;
    };

    int64_t xmlListLen;
    TTHValue xmlRoot;
    int64_t bzXmlListLen;
    TTHValue bzXmlRoot;
    unique_ptr<File> bzXmlRef;

    bool xmlDirty;
    bool forceXmlRefresh; /// bypass the 15-minutes guard
    bool refreshDirs;
    bool update;
    bool initial;

    int listN;

    Atomic<bool,memory_ordering_strong> refreshing;

    uint64_t lastXmlUpdate;
    uint64_t lastFullUpdate;

    mutable CriticalSection cs;

    // List of root directory items
    typedef std::list<Directory::Ptr> DirList;
    DirList directories;

    /** Map real name to virtual name - multiple real names may be mapped to a single virtual one */
    StringMap shares;

#ifdef WITH_DHT
    friend class ::dht::IndexManager;
#endif

    typedef unordered_map<TTHValue, Directory::File::Set::const_iterator> HashFileMap;
    typedef HashFileMap::iterator HashFileIter;

    HashFileMap tthIndex;

    BloomFilter<5> bloom;

    Directory::File::Set::const_iterator findFile(const string& virtualFile) const;

    Directory::Ptr buildTree(const string& aName, const Directory::Ptr& aParent);
    bool checkHidden(const string& aName) const;

    void rebuildIndices();

    void updateIndices(Directory& aDirectory);
    void updateIndices(Directory& dir, const decltype(std::declval<Directory>().files.begin())& i);

    Directory::Ptr merge(const Directory::Ptr& directory);

    void generateXmlList();
    bool loadCache() noexcept;
    DirList::const_iterator getByVirtual(const string& virtualName) const noexcept;
    pair<Directory::Ptr, string> splitVirtual(const string& virtualPath) const;

    string findRealRoot(const string& virtualRoot, const string& virtualLeaf) const;

    Directory::Ptr getDirectory(const string& fname);

    virtual int run();

    // QueueManagerListener
    virtual void on(QueueManagerListener::FileMoved, const string& realPath) noexcept;
    // HashManagerListener
    virtual void on(HashManagerListener::TTHDone, const string& realPath, const TTHValue& root) noexcept;

    // SettingsManagerListener
    virtual void on(SettingsManagerListener::Save, SimpleXML& xml) noexcept {
        save(xml);
    }
    virtual void on(SettingsManagerListener::Load, SimpleXML& xml) noexcept {
        load(xml);
    }

    // TimerManagerListener
    virtual void on(TimerManagerListener::Minute, uint64_t tick) noexcept;
    void load(SimpleXML& aXml);
    void save(SimpleXML& aXml);

};

} // namespace dcpp
