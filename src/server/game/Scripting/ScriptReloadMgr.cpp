/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptReloadMgr.h"
#include "Errors.h"

#ifndef TRINITY_API_USE_DYNAMIC_LINKING

// This method should never be called
std::shared_ptr<ModuleReference>
    ScriptReloadMgr::AcquireModuleReferenceOfContext(std::string const& /*context*/)
{
    WPAbort();
}

// Returns the empty implemented ScriptReloadMgr
ScriptReloadMgr* ScriptReloadMgr::instance()
{
    static ScriptReloadMgr instance;
    return &instance;
}

#else

#include "BuiltInConfig.h"
#include "Config.h"
#include "GitRevision.h"
#include "CryptoHash.h"
#include "Duration.h"
#include "Log.h"
#include "MPSCQueue.h"
#include "Optional.h"
#include "Regex.h"
#include "ScriptMgr.h"
#include "StartProcess.h"
#include "Timer.h"
#include "Util.h"
#include "World.h"
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/system/system_error.hpp>
#include <efsw/efsw.hpp>
#include <algorithm>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = boost::filesystem;

#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
    #include <windows.h>
#else // Posix and Apple
    #include <dlfcn.h>
#endif

// Promote the sScriptReloadMgr to a HotSwapScriptReloadMgr
// in this compilation unit.
#undef sScriptReloadMgr
#define sScriptReloadMgr static_cast<HotSwapScriptReloadMgr*>(ScriptReloadMgr::instance())

// Returns "" on Windows and "lib" on posix.
static char const* GetSharedLibraryPrefix()
{
#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
    return "";
#else // Posix
    return "lib";
#endif
}

// Returns "dll" on Windows, "dylib" on OS X, and "so" on posix.
static char const* GetSharedLibraryExtension()
{
#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
    return "dll";
#elif TRINITY_PLATFORM == TRINITY_PLATFORM_APPLE
    return "dylib";
#else // Posix
    return "so";
#endif
}

#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
typedef HMODULE HandleType;
#else // Posix
typedef void* HandleType;
#endif

static fs::path GetDirectoryOfExecutable()
{
    return boost::dll::program_location().parent_path();
}

class SharedLibraryUnloader
{
public:
    explicit SharedLibraryUnloader(fs::path path)
        : path_(std::move(path)) { }
    SharedLibraryUnloader(fs::path path, Optional<fs::path> cache_path)
        : path_(std::move(path)), cache_path_(std::move(cache_path)) { }

    void operator() (HandleType handle) const
    {
        // Unload the associated shared library.
#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
        bool success = (FreeLibrary(handle) != 0);
#else // Posix
        bool success = (dlclose(handle) == 0);
#endif

        if (!success)
        {
            TC_LOG_ERROR("scripts.hotswap", "Failed to unload (syscall) the shared library \"{}\".",
                path_.generic_string());

            return;
        }

        /// When the shared library was cached delete it's shared version
        if (cache_path_)
        {
            boost::system::error_code error;
            if (!fs::remove(*cache_path_, error))
            {
                TC_LOG_ERROR("scripts.hotswap", "Failed to delete the cached shared library \"{}\" ({}).",
                    cache_path_->generic_string(), error.message());

                return;
            }

            TC_LOG_TRACE("scripts.hotswap", "Lazy unloaded the shared library \"{}\" "
                         "and deleted it's cached version at \"{}\"",
                         path_.generic_string(), cache_path_->generic_string());
        }
        else
        {
            TC_LOG_TRACE("scripts.hotswap", "Lazy unloaded the shared library \"{}\".",
                path_.generic_string());
        }
    }

private:
    fs::path const path_;
    Optional<fs::path> const cache_path_;
};

typedef std::unique_ptr<typename std::remove_pointer<HandleType>::type, SharedLibraryUnloader> HandleHolder;

typedef char const* (*GetScriptModuleRevisionHashType)();
typedef void (*AddScriptsType)();
typedef char const* (*GetScriptModuleType)();
typedef char const* (*GetBuildDirectiveType)();

class ScriptModule
    : public ModuleReference
{
public:
    explicit ScriptModule(HandleHolder handle, GetScriptModuleRevisionHashType getScriptModuleRevisionHash,
                 AddScriptsType addScripts, GetScriptModuleType getScriptModule,
                 GetBuildDirectiveType getBuildDirective, fs::path const& path)
        : _handle(std::forward<HandleHolder>(handle)), _getScriptModuleRevisionHash(getScriptModuleRevisionHash),
          _addScripts(addScripts), _getScriptModule(getScriptModule),
          _getBuildDirective(getBuildDirective), _path(path) { }

    ScriptModule(ScriptModule const&) = delete;
    ScriptModule(ScriptModule&& right) = delete;

    ScriptModule& operator= (ScriptModule const&) = delete;
    ScriptModule& operator= (ScriptModule&& right) = delete;

    static Optional<std::shared_ptr<ScriptModule>>
        CreateFromPath(fs::path const& path, Optional<fs::path> cache_path);

    static void ScheduleDelayedDelete(ScriptModule* module);

    char const* GetScriptModuleRevisionHash() const override
    {
        return _getScriptModuleRevisionHash();
    }

    void AddScripts() const
    {
        return _addScripts();
    }

    char const* GetScriptModule() const override
    {
        return _getScriptModule();
    }

    char const* GetBuildDirective() const
    {
        return _getBuildDirective();
    }

    fs::path const& GetModulePath() const override
    {
        return _path;
    }

private:
    HandleHolder _handle;

    GetScriptModuleRevisionHashType _getScriptModuleRevisionHash;
    AddScriptsType _addScripts;
    GetScriptModuleType _getScriptModule;
    GetBuildDirectiveType _getBuildDirective;

    fs::path _path;
};

template<typename Fn>
static bool GetFunctionFromSharedLibrary(HandleType handle, std::string const& name, Fn& fn)
{
#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
    fn = reinterpret_cast<Fn>(GetProcAddress(handle, name.c_str()));
#else // Posix
    fn = reinterpret_cast<Fn>(dlsym(handle, name.c_str()));
#endif
    return fn != nullptr;
}

// Load a shared library from the given path.
Optional<std::shared_ptr<ScriptModule>>
    ScriptModule::CreateFromPath(fs::path const& path, Optional<fs::path> cache_path)
{
    auto const load_path = [&] () -> fs::path {
        if (cache_path)
            return *cache_path;
        else
            return path;
    }();

#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
    HandleType handle = LoadLibrary(load_path.generic_string().c_str());
#else // Posix
    HandleType handle = dlopen(load_path.generic_string().c_str(), RTLD_LAZY);
#endif

    if (!handle)
    {
        if (cache_path)
        {
            TC_LOG_ERROR("scripts.hotswap", "Could not dynamic load the shared library \"{}\" "
                         "(the library is cached at {})",
                         path.generic_string(), cache_path->generic_string());
        }
        else
        {
            TC_LOG_ERROR("scripts.hotswap", "Could not dynamic load the shared library \"{}\".",
                         path.generic_string());
        }

        return {};
    }

    // Use RAII to release the library on failure.
    HandleHolder holder(handle, SharedLibraryUnloader(path, std::move(cache_path)));

    GetScriptModuleRevisionHashType getScriptModuleRevisionHash;
    AddScriptsType addScripts;
    GetScriptModuleType getScriptModule;
    GetBuildDirectiveType getBuildDirective;

    if (GetFunctionFromSharedLibrary(handle, "GetScriptModuleRevisionHash", getScriptModuleRevisionHash) &&
        GetFunctionFromSharedLibrary(handle, "AddScripts", addScripts) &&
        GetFunctionFromSharedLibrary(handle, "GetScriptModule", getScriptModule) &&
        GetFunctionFromSharedLibrary(handle, "GetBuildDirective", getBuildDirective))
    {
        auto module = new ScriptModule(std::move(holder), getScriptModuleRevisionHash,
            addScripts, getScriptModule, getBuildDirective, path);

        // Unload the module at the next update tick as soon as all references are removed
        return std::shared_ptr<ScriptModule>(module, ScheduleDelayedDelete);
    }
    else
    {
        TC_LOG_ERROR("scripts.hotswap", "Could not extract all required functions from the shared library \"{}\"!",
            path.generic_string());

        return {};
    }
}

static bool HasValidScriptModuleName(std::string const& name)
{
    // Detects scripts_NAME.dll's / .so's
    static Trinity::regex const regex(
        Trinity::StringFormat("^{}[sS]cripts_[a-zA-Z0-9_]+\\.{}$",
            GetSharedLibraryPrefix(),
            GetSharedLibraryExtension()));

    return Trinity::regex_match(name, regex);
}

/// File watcher responsible for watching shared libraries
class LibraryUpdateListener : public efsw::FileWatchListener
{
public:
    LibraryUpdateListener() { }
    virtual ~LibraryUpdateListener() { }

    void handleFileAction(efsw::WatchID /*watchid*/, std::string const& dir,
        std::string const& filename, efsw::Action action, std::string oldFilename = "") final override;
};

static LibraryUpdateListener libraryUpdateListener;

/// File watcher responsible for watching source files
class SourceUpdateListener : public efsw::FileWatchListener
{
    fs::path const path_;

    std::string const script_module_name_;

    efsw::WatchID const watcher_id_;

public:
    explicit SourceUpdateListener(fs::path path, std::string script_module_name);

    virtual ~SourceUpdateListener();

    void handleFileAction(efsw::WatchID /*watchid*/, std::string const& dir,
        std::string const& filename, efsw::Action action, std::string oldFilename = "") final override;
};

namespace std
{
    template <>
    struct hash<fs::path>
    {
        hash<string> hasher;

        std::size_t operator()(fs::path const& key) const noexcept
        {
            return hasher(key.generic_string());
        }
    };
}

/// Invokes a synchronous CMake process with the given arguments
template<typename... T>
static int InvokeCMakeCommand(T&&... args)
{
    auto const executable = BuiltInConfig::GetCMakeCommand();
    return Trinity::StartProcess(executable, {
        std::forward<T>(args)...
    }, "scripts.hotswap");
}

/// Invokes an asynchronous CMake process with the given arguments
template<typename... T>
static std::shared_ptr<Trinity::AsyncProcessResult> InvokeAsyncCMakeCommand(T&&... args)
{
    auto const executable = BuiltInConfig::GetCMakeCommand();
    return Trinity::StartAsyncProcess(executable, {
        std::forward<T>(args)...
    }, "scripts.hotswap");
}

/// Calculates the C++ project name of the given module which is
/// the lowercase string of scripts_${module}.
static std::string CalculateScriptModuleProjectName(std::string const& module)
{
    std::string module_project = "scripts_" + module;
    strToLower(module_project);

    return module_project;
}

/// Returns false when there isn't any attached debugger to the process which
/// could block the rebuild of new shared libraries.
static bool IsDebuggerBlockingRebuild()
{
#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
    if (IsDebuggerPresent())
        return true;
#endif
    return false;
}

/// ScriptReloadMgr which is used when dynamic linking is enabled
///
/// This class manages shared library loading/unloading through watching
/// the script module directory. Loaded shared libraries are mirrored
/// into a cache subdirectory to allow lazy unloading as long as
/// the shared library is still used which is useful for scripts
/// which can't be instantly replaced like spells or instances.
/// Several modules which reference different versions can be kept loaded
/// to serve scripts of different versions to entities and spells.
///
/// Also this class invokes rebuilds as soon as the source of loaded
/// scripts change and installs the modules correctly through CMake.
class HotSwapScriptReloadMgr final
    : public ScriptReloadMgr
{
    friend class ScriptReloadMgr;
    friend class SourceUpdateListener;

    /// Reflects a queued change on a shared library or shared library
    /// which is waiting for processing
    enum class ChangeStateRequest : uint8
    {
        CHANGE_REQUEST_ADDED,
        CHANGE_REQUEST_MODIFIED,
        CHANGE_REQUEST_REMOVED
    };

    /// Reflects a running job of an invoked asynchronous external process
    enum class BuildJobType : uint8
    {
        BUILD_JOB_NONE,
        BUILD_JOB_RERUN_CMAKE,
        BUILD_JOB_COMPILE,
        BUILD_JOB_INSTALL,
    };

    // Represents a job which was invoked through a source or shared library change
    class BuildJob
    {
        // Script module which is processed in the current running job
        std::string script_module_name_;
        // The C++ project name of the module which is processed
        std::string script_module_project_name_;
        // The build directive of the current module which is processed
        // like "Release" or "Debug". The build directive from the
        // previous same module is used if there was any.
        std::string script_module_build_directive_;
        // The time where the build job started
        uint32 start_time_;

        // Type of the current running job
        BuildJobType type_;
        // The async process result of the current job
        std::shared_ptr<Trinity::AsyncProcessResult> async_result_;

    public:
        explicit BuildJob(std::string script_module_name, std::string script_module_project_name,
                 std::string script_module_build_directive)
            : script_module_name_(std::move(script_module_name)),
              script_module_project_name_(std::move(script_module_project_name)),
              script_module_build_directive_(std::move(script_module_build_directive)),
              start_time_(getMSTime()), type_(BuildJobType::BUILD_JOB_NONE) { }

        bool IsValid() const
        {
            return type_ != BuildJobType::BUILD_JOB_NONE;
        }

        std::string const& GetModuleName() const { return script_module_name_; }

        std::string const& GetProjectName() const { return script_module_project_name_; }

        std::string const& GetBuildDirective() const { return script_module_build_directive_; }

        uint32 GetTimeFromStart() const { return GetMSTimeDiffToNow(start_time_); }

        BuildJobType GetType() const { return type_; }

        std::shared_ptr<Trinity::AsyncProcessResult> const& GetProcess() const
        {
            ASSERT(async_result_, "Tried to access an empty process handle!");
            return async_result_;
        }

        /// Updates the current running job with the given type and async result
        void UpdateCurrentJob(BuildJobType type,
                              std::shared_ptr<Trinity::AsyncProcessResult> async_result)
        {
            ASSERT(type != BuildJobType::BUILD_JOB_NONE, "None isn't allowed here!");
            ASSERT(async_result, "The async result must not be empty!");

            type_ = type;
            async_result_ = std::move(async_result);
        }
    };

    /// Base class for lockfree asynchronous messages to the script reloader
    class ScriptReloaderMessage
    {
    public:
        virtual ~ScriptReloaderMessage() { }

        /// Invoke this function to run a message thread safe on the reloader
        virtual void operator() (HotSwapScriptReloadMgr* reloader) = 0;
    };

    /// Implementation class which wraps functional types and dispatches
    /// it in the overwritten implementation of the reloader messages.
    template<typename T>
    class ScriptReloaderMessageImplementation
        : public ScriptReloaderMessage
    {
        T dispatcher_;

    public:
        explicit ScriptReloaderMessageImplementation(T dispatcher)
            : dispatcher_(std::move(dispatcher)) { }

        void operator() (HotSwapScriptReloadMgr* reloader) final override
        {
            dispatcher_(reloader);
        }
    };

    /// Uses the given functional type and creates a asynchronous reloader
    /// message on the heap, which requires deletion.
    template<typename T>
    auto MakeMessage(T&& dispatcher)
        -> ScriptReloaderMessageImplementation<typename std::decay<T>::type>*
    {
        return new ScriptReloaderMessageImplementation<typename std::decay<T>::type>
            (std::forward<T>(dispatcher));
    }

public:
    HotSwapScriptReloadMgr()
        : _libraryWatcher(-1), _unique_library_name_counter(0),
          _last_time_library_changed(0), _last_time_sources_changed(0),
          _last_time_user_informed(0), terminate_early(false) { }

    virtual ~HotSwapScriptReloadMgr()
    {
        // Delete all messages
        ScriptReloaderMessage* message;
        while (_messages.Dequeue(message))
            delete message;
    }

    /// Returns the absolute path to the script module directory
    static fs::path GetLibraryDirectory()
    {
        // When an absolute path is given in the config use it,
        // otherwise interpret paths relative to the executable.
        fs::path path(sConfigMgr->GetStringDefault("HotSwap.ScriptDir", "scripts"));
        if (path.is_absolute())
            return path;
        else
            return fs::absolute(path, GetDirectoryOfExecutable());
    }

    /// Returns the absolute path to the scripts directory in the source tree.
    static fs::path GetSourceDirectory()
    {
        fs::path dir = BuiltInConfig::GetSourceDirectory();
        dir /= "src";
        dir /= "server";
        dir /= "scripts";
        return dir;
    }

    /// Initializes the file watchers and loads all existing shared libraries
    /// into the running server.
    void Initialize() final override
    {
        if (!sWorld->getBoolConfig(CONFIG_HOTSWAP_ENABLED))
            return;

        if (BuiltInConfig::GetBuildDirectory().find(" ") != std::string::npos)
        {
            TC_LOG_ERROR("scripts.hotswap", "Your build directory path \"{}\" "
                "contains spaces, which isn't allowed for compatibility reasons! "
                "You need to create a build directory which doesn't contain any space character "
                "in it's path!",
                BuiltInConfig::GetBuildDirectory());

            return;
        }

        {
            auto const library_directory = GetLibraryDirectory();
            if (!fs::exists(library_directory) || !fs::is_directory(library_directory))
            {
                TC_LOG_ERROR("scripts.hotswap", "Library directory \"{}\" doesn't exist!.",
                    library_directory.generic_string());
                return;
            }
        }

        temporary_cache_path_ = CalculateTemporaryCachePath();

        // We use the boost filesystem function versions which accept
        // an error code to prevent it from throwing exceptions.
        boost::system::error_code code;
        if ((!fs::exists(temporary_cache_path_, code)
             || (fs::remove_all(temporary_cache_path_, code) > 0)) &&
             !fs::create_directories(temporary_cache_path_, code))
        {
            TC_LOG_ERROR("scripts.hotswap", "Couldn't create the cache directory at \"{}\".",
                temporary_cache_path_.generic_string());

            return;
        }

        // Used to silent compiler warnings
        (void)code;

        // Correct the CMake prefix when needed
        if (sWorld->getBoolConfig(CONFIG_HOTSWAP_PREFIX_CORRECTION_ENABLED))
            DoCMakePrefixCorrectionIfNeeded();

        InitializeDefaultLibraries();
        InitializeFileWatchers();
    }

    /// Needs to be called periodically from the worldserver loop
    /// to invoke queued actions like module loading/unloading and script
    /// compilation.
    /// This method should be invoked from a thread safe point to
    /// prevent misbehavior.
    void Update() final override
    {
        // Consume all messages
        ScriptReloaderMessage* message;
        while (_messages.Dequeue(message))
        {
            (*message)(this);
            delete message;
        }

        DispatchRunningBuildJobs();
        DispatchModuleChanges();
    }

    /// Unloads the manager and cancels all runnings jobs immediately
    void Unload() final override
    {
        if (_libraryWatcher >= 0)
        {
            _fileWatcher.removeWatch(_libraryWatcher);
            _libraryWatcher = -1;
        }

        // If a build is in progress cancel it
        if (_build_job)
        {
            _build_job->GetProcess()->Terminate();
            _build_job.reset();
        }

        // Release all strong references to script modules
        // to trigger unload actions as early as possible,
        // otherwise the worldserver will crash on exit.
        _running_script_modules.clear();
    }

    /// Queue's a thread safe message to the reloader which is executed on
    /// the next world server update tick.
    template<typename T>
    void QueueMessage(T&& message)
    {
       _messages.Enqueue(MakeMessage(std::forward<T>(message)));
    }

    /// Queues an action which marks the given shared library as changed
    /// which will add, unload or reload it at the next world update tick.
    /// This method is thread safe.
    void QueueSharedLibraryChanged(fs::path const& path)
    {
         _last_time_library_changed = getMSTime();
         _libraries_changed.insert(path);
    }

    /// Queues a notification that a source file was added
    /// This method is thread unsafe.
    void QueueAddSourceFile(std::string const& module_name, fs::path const& path)
    {
        UpdateSourceChangeRequest(module_name, path, ChangeStateRequest::CHANGE_REQUEST_ADDED);
    }

    /// Queues a notification that a source file was modified
    /// This method is thread unsafe.
    void QueueModifySourceFile(std::string const& module_name, fs::path const& path)
    {
        UpdateSourceChangeRequest(module_name, path, ChangeStateRequest::CHANGE_REQUEST_MODIFIED);
    }

    /// Queues a notification that a source file was removed
    /// This method is thread unsafe.
    void QueueRemoveSourceFile(std::string const& module_name, fs::path const& path)
    {
        UpdateSourceChangeRequest(module_name, path, ChangeStateRequest::CHANGE_REQUEST_REMOVED);
    }

private:
    // Loads all shared libraries which are contained in the
    // scripts directory on startup.
    void InitializeDefaultLibraries()
    {
        fs::path const libraryDirectory(GetLibraryDirectory());
        fs::directory_iterator const dir_end;

        uint32 count = 0;

        // Iterate through all shared libraries in the script directory and load it
        for (fs::directory_iterator dir_itr(libraryDirectory); dir_itr != dir_end ; ++dir_itr)
            if (fs::is_regular_file(dir_itr->path()) && HasValidScriptModuleName(dir_itr->path().filename().generic_string()))
            {
                TC_LOG_INFO("scripts.hotswap", "Loading script module \"{}\"...",
                    dir_itr->path().filename().generic_string());

                // Don't swap the script context to do bulk loading
                ProcessLoadScriptModule(dir_itr->path(), false);
                ++count;
            }

        TC_LOG_INFO("scripts.hotswap", ">> Loaded {} script modules.", count);
    }

    // Initialize all enabled file watchers.
    // Needs to be called after InitializeDefaultLibraries()!
    void InitializeFileWatchers()
    {
        _libraryWatcher = _fileWatcher.addWatch(GetLibraryDirectory().generic_string(), &libraryUpdateListener, false);
        if (_libraryWatcher >= 0)
        {
            TC_LOG_INFO("scripts.hotswap", ">> Library reloader is listening on \"{}\".",
                GetLibraryDirectory().generic_string());
        }
        else
        {
            TC_LOG_ERROR("scripts.hotswap", "Failed to initialize the library reloader on \"{}\".",
                GetLibraryDirectory().generic_string());
        }

        _fileWatcher.watch();
    }

    static fs::path CalculateTemporaryCachePath()
    {
        auto path = fs::temp_directory_path();
        path /= Trinity::StringFormat("tc_script_cache_{}_{}",
            GitRevision::GetBranch(),
            ByteArrayToHexStr(Trinity::Crypto::SHA1::GetDigestOf(sConfigMgr->GetFilename())));

        return path;
    }

    fs::path GenerateUniquePathForLibraryInCache(fs::path path)
    {
        ASSERT(!temporary_cache_path_.empty(),
               "The temporary cache path wasn't set!");

        // Create the cache path and increment the library counter to use an unique name for each library
        auto cache_path = temporary_cache_path_;
        cache_path /= Trinity::StringFormat("{}.{}{}",
            path.stem().generic_string(),
            _unique_library_name_counter++,
            path.extension().generic_string());

        return cache_path;
    }

    /// Updates the current state of the given source path
    void UpdateSourceChangeRequest(std::string const& module_name,
                                   fs::path const& path,
                                   ChangeStateRequest state)
    {
        _last_time_sources_changed = getMSTime();

        // Write when there is no module with the given name known
        auto module_itr = _sources_changed.find(module_name);

        // When the file was modified it's enough to mark the module as
        // dirty by initializing the associated map.
        if (module_itr == _sources_changed.end())
            module_itr = _sources_changed.insert(std::make_pair(
                module_name, decltype(_sources_changed)::mapped_type{})).first;

        // Leave when the file was just modified as explained above
        if (state == ChangeStateRequest::CHANGE_REQUEST_MODIFIED)
            return;

        // Insert when the given path isn't existent
        auto const itr = module_itr->second.find(path);
        if (itr == module_itr->second.end())
        {
            module_itr->second.insert(std::make_pair(path, state));
            return;
        }

        ASSERT((itr->second == ChangeStateRequest::CHANGE_REQUEST_ADDED)
               || (itr->second == ChangeStateRequest::CHANGE_REQUEST_REMOVED),
               "Stored value is invalid!");

        ASSERT((state == ChangeStateRequest::CHANGE_REQUEST_ADDED)
               || (state == ChangeStateRequest::CHANGE_REQUEST_REMOVED),
               "The given state is invalid!");

        ASSERT(state != itr->second,
               "Tried to apply a state which is stored already!");

        module_itr->second.erase(itr);
    }

    /// Called periodically on the worldserver tick to process all
    /// load/unload/reload requests of shared libraries.
    void DispatchModuleChanges()
    {
        // When there are no libraries to change return
        if (_libraries_changed.empty())
            return;

        // Wait some time after changes to catch bulk changes
        if (GetMSTimeDiffToNow(_last_time_library_changed) < 500)
            return;

        for (auto const& path : _libraries_changed)
        {
            bool const is_running =
                _running_script_module_names.find(path) != _running_script_module_names.end();

            bool const exists = fs::exists(path);

            if (is_running)
            {
                if (exists)
                    ProcessReloadScriptModule(path);
                else
                    ProcessUnloadScriptModule(path);
            }
            else if (exists)
                ProcessLoadScriptModule(path);
        }

        _libraries_changed.clear();
    }

    void ProcessLoadScriptModule(fs::path const& path, bool swap_context = true)
    {
        ASSERT(_running_script_module_names.find(path) == _running_script_module_names.end(),
               "Can't load a module which is running already!");

        // Copy the shared library into a cache
        auto cache_path = GenerateUniquePathForLibraryInCache(path);

        {
            boost::system::error_code code;
            fs::copy_file(path, cache_path, code);
            if (code)
            {
                TC_LOG_FATAL("scripts.hotswap", ">> Failed to create cache entry for module "
                    "\"{}\" at \"{}\" with reason (\"{}\")!",
                    path.filename().generic_string(), cache_path.generic_string(),
                    code.message());

                // Find a better solution for this but it's much better
                // to start the core without scripts
                std::this_thread::sleep_for(std::chrono::seconds(5));
                ABORT();
                return;
            }

            TC_LOG_TRACE("scripts.hotswap", ">> Copied the shared library \"{}\" to \"{}\" for caching.",
                path.filename().generic_string(), cache_path.generic_string());
        }

        auto module = ScriptModule::CreateFromPath(path, cache_path);
        if (!module)
        {
            TC_LOG_FATAL("scripts.hotswap", ">> Failed to load script module \"{}\"!",
                path.filename().generic_string());

            // Find a better solution for this but it's much better
            // to start the core without scripts
            std::this_thread::sleep_for(std::chrono::seconds(5));
            ABORT();
            return;
        }

        // Limit the git revision hash to 7 characters.
        std::string module_revision((*module)->GetScriptModuleRevisionHash());
        if (module_revision.size() >= 7)
            module_revision = module_revision.substr(0, 7);

        std::string const module_name = (*module)->GetScriptModule();
        TC_LOG_INFO("scripts.hotswap", ">> Loaded script module \"{}\" (\"{}\" - {}).",
            path.filename().generic_string(), module_name, module_revision);

        if (module_revision.empty())
        {
            TC_LOG_WARN("scripts.hotswap", ">> Script module \"{}\" has an empty revision hash!",
                path.filename().generic_string());
        }
        else
        {
            // Trim the revision hash
            std::string my_revision_hash = GitRevision::GetHash();
            std::size_t const trim = std::min(module_revision.size(), my_revision_hash.size());
            my_revision_hash = my_revision_hash.substr(0, trim);
            module_revision = module_revision.substr(0, trim);

            if (my_revision_hash != module_revision)
            {
                TC_LOG_WARN("scripts.hotswap", ">> Script module \"{}\" has a different revision hash! "
                    "Binary incompatibility could lead to unknown behaviour!", path.filename().generic_string());
            }
        }

        {
            auto const itr = _running_script_modules.find(module_name);
            if (itr != _running_script_modules.end())
            {
                TC_LOG_ERROR("scripts.hotswap", ">> Attempt to load a module twice \"{}\" (loaded module is at {})!",
                    path.generic_string(), itr->second.first->GetModulePath().generic_string());

                return;
            }
        }

        // Create the source listener
        auto listener = std::make_unique<SourceUpdateListener>(
            sScriptReloadMgr->GetSourceDirectory() / module_name,
            module_name);

        // Store the module
        _known_modules_build_directives.insert(std::make_pair(module_name, (*module)->GetBuildDirective()));
        _running_script_modules.insert(std::make_pair(module_name,
            std::make_pair(*module, std::move(listener))));
        _running_script_module_names.insert(std::make_pair(path, module_name));

        // Process the script loading after the module was registered correctly (#17557).
        sScriptMgr->SetScriptContext(module_name);
        (*module)->AddScripts();
        TC_LOG_TRACE("scripts.hotswap", ">> Registered all scripts of module {}.", module_name);

        if (swap_context)
            sScriptMgr->SwapScriptContext();
    }

    void ProcessReloadScriptModule(fs::path const& path)
    {
        ProcessUnloadScriptModule(path, false);
        ProcessLoadScriptModule(path);
    }

    void ProcessUnloadScriptModule(fs::path const& path, bool finish = true)
    {
        auto const itr = _running_script_module_names.find(path);

        ASSERT(itr != _running_script_module_names.end(),
               "Can't unload a module which isn't running!");

        // Unload the script context
        sScriptMgr->ReleaseScriptContext(itr->second);

        if (finish)
            sScriptMgr->SwapScriptContext();

        TC_LOG_INFO("scripts.hotswap", "Released script module \"{}\" (\"{}\")...",
            path.filename().generic_string(), itr->second);

        // Unload the script module
        auto ref = _running_script_modules.find(itr->second);
        ASSERT(ref != _running_script_modules.end() &&
               "Expected the script reference to be present!");

        // Yield a message when there are other owning references to
        // the module which prevents it from unloading.
        // The module will be unloaded once all scripts provided from the module
        // are destroyed.
        if (ref->second.first.use_count() != 1)
        {
            TC_LOG_INFO("scripts.hotswap",
                "Script module {} is still used by {} spell, aura or instance scripts. "
                "Will lazy unload the module once all scripts stopped using it, "
                "to use the latest version of an edited script unbind yourself from "
                "the instance or re-cast the spell.",
                ref->second.first->GetScriptModule(), ref->second.first.use_count() - 1);
        }

        // Remove the owning reference from the reloader
        _running_script_modules.erase(ref);
        _running_script_module_names.erase(itr);
    }

    /// Called periodically on the worldserver tick to process all recompile
    /// requests. This method invokes one build or install job at the time
    void DispatchRunningBuildJobs()
    {
        if (_build_job)
        {
            // Terminate the current build job when an associated source was changed
            // while compiling and the terminate early option is enabled.
            if (sWorld->getBoolConfig(CONFIG_HOTSWAP_EARLY_TERMINATION_ENABLED))
            {
                if (!terminate_early && _sources_changed.find(_build_job->GetModuleName()) != _sources_changed.end())
                {
                    /*
                    FIXME: Currently crashes the server
                    TC_LOG_INFO("scripts.hotswap", "Terminating the running build of module \"{}\"...",
                                _build_job->GetModuleName());

                    _build_job->GetProcess()->Terminate();
                    _build_job.reset();

                    // Continue with the default execution path
                    DispatchRunningBuildJobs();
                    return;
                    */

                    terminate_early = true;
                    return;
                }
            }

            // Wait for the current build job to finish, if the job finishes in time
            // evaluate it and continue with the next one.
            if (_build_job->GetProcess()->GetFutureResult().
                    wait_for(0s) == std::future_status::ready)
                ProcessReadyBuildJob();
            else
                return; // Return when the job didn't finish in time

            // Skip this cycle when the previous job scheduled a new one
            if (_build_job)
                return;
        }

        // Avoid burst updates through waiting for a short time after changes
        if ((_last_time_sources_changed != 0) &&
            (GetMSTimeDiffToNow(_last_time_sources_changed) < 500))
            return;

        // If the changed sources are empty do nothing
        if (_sources_changed.empty())
            return;

        // Wait until are attached debugger were detached.
        if (IsDebuggerBlockingRebuild())
        {
            if ((_last_time_user_informed == 0) ||
                (GetMSTimeDiffToNow(_last_time_user_informed) > 7500))
            {
                _last_time_user_informed = getMSTime();

                // Informs the user that the attached debugger is blocking the automatic script rebuild.
                TC_LOG_INFO("scripts.hotswap", "Your attached debugger is blocking the TrinityCore "
                    "automatic script rebuild, please detach it!");
            }

            return;
        }

        // Find all source files of a changed script module and removes
        // it from the changed source list, invoke the build afterwards.
        bool rebuild_buildfiles;
        auto module_name = [&]
        {
            auto itr = _sources_changed.begin();
            auto name = itr->first;
            rebuild_buildfiles = !itr->second.empty();

            if (sLog->ShouldLog("scripts.hotswap", LogLevel::LOG_LEVEL_TRACE))
                for (auto const& entry : itr->second)
                {
                    TC_LOG_TRACE("scripts.hotswap", "Source file {} was {}.",
                        entry.first.generic_string(),
                        ((entry.second == ChangeStateRequest::CHANGE_REQUEST_ADDED) ?
                            "added" : "removed"));
                }

            _sources_changed.erase(itr);
            return name;
        }();

        // Erase the added delete history all modules when we
        // invoke a cmake rebuild since we add all
        // added files of other modules to the build as well
        if (rebuild_buildfiles)
        {
            for (auto& entry : _sources_changed)
                entry.second.clear();
        }

        ASSERT(!module_name.empty(),
               "The current module name is invalid!");

        TC_LOG_INFO("scripts.hotswap", "Recompiling Module \"{}\"...",
            module_name);

        // Calculate the project name of the script module
        auto project_name = CalculateScriptModuleProjectName(module_name);

        // Find the best build directive for the module
        auto build_directive = [&] () -> std::string
        {
            auto directive = sConfigMgr->GetStringDefault("HotSwap.ReCompilerBuildType", "");
            if (!directive.empty())
                return directive;

            auto const itr = _known_modules_build_directives.find(module_name);
            if (itr != _known_modules_build_directives.end())
                return itr->second;
            else // If no build directive of the module was found use the one from the game library
                return TRINITY_BUILD_TYPE;
        }();

        // Initiate the new build job
        _build_job = BuildJob(std::move(module_name),
            std::move(project_name), std::move(build_directive));

        // Rerun CMake when we need to recreate the build files
        if (rebuild_buildfiles
            && sWorld->getBoolConfig(CONFIG_HOTSWAP_BUILD_FILE_RECREATION_ENABLED))
            DoRerunCMake();
        else
            DoCompileCurrentProcessedModule();
    }

    void ProcessReadyBuildJob()
    {
        ASSERT(_build_job->IsValid(), "Invalid build job!");

        // Retrieve the result
        auto const error = _build_job->GetProcess()->GetFutureResult().get();

        if (terminate_early)
        {
            _build_job.reset();
            terminate_early = false;
            return;
        }

        switch (_build_job->GetType())
        {
            case BuildJobType::BUILD_JOB_RERUN_CMAKE:
            {
                if (!error)
                {
                    TC_LOG_INFO("scripts.hotswap", ">> Successfully updated the build files!");
                }
                else
                {
                    TC_LOG_INFO("scripts.hotswap", ">> Failed to update the build files at \"{}\", "
                                "it's possible that recently added sources are not included "
                                "in your next builds, rerun CMake manually.",
                                BuiltInConfig::GetBuildDirectory());
                }
                // Continue with building the changes sources
                DoCompileCurrentProcessedModule();
                return;
            }
            case BuildJobType::BUILD_JOB_COMPILE:
            {
                if (!error) // Build was successful
                {
                    if (sWorld->getBoolConfig(CONFIG_HOTSWAP_INSTALL_ENABLED))
                    {
                        // Continue with the installation when it's enabled
                        TC_LOG_INFO("scripts.hotswap",
                                    ">> Successfully build module {}, continue with installing...",
                                    _build_job->GetModuleName());

                        DoInstallCurrentProcessedModule();
                        return;
                    }

                    // Skip the installation because it's disabled in config
                    TC_LOG_INFO("scripts.hotswap",
                        ">> Successfully build module {}, skipped the installation.",
                        _build_job->GetModuleName());
                }
                else // Build wasn't successful
                {
                    TC_LOG_ERROR("scripts.hotswap",
                        ">> The build of module {} failed! See the log for details.",
                        _build_job->GetModuleName());
                }
                break;
            }
            case BuildJobType::BUILD_JOB_INSTALL:
            {
                if (!error)
                {
                    // Installation was successful
                    TC_LOG_INFO("scripts.hotswap", ">> Successfully installed module {} in {}s",
                        _build_job->GetModuleName(),
                        _build_job->GetTimeFromStart() / IN_MILLISECONDS);
                }
                else
                {
                    // Installation wasn't successful
                    TC_LOG_INFO("scripts.hotswap",
                        ">> The installation of module {} failed! See the log for details.",
                        _build_job->GetModuleName());
                }
                break;
            }
            default:
                break;
        }

        // Clear the current job
        _build_job.reset();
    }

    /// Reruns CMake asynchronously over the build directory
    void DoRerunCMake()
    {
        ASSERT(_build_job, "There isn't any active build job!");

        TC_LOG_INFO("scripts.hotswap", "Rerunning CMake because there were sources added or removed...");

        _build_job->UpdateCurrentJob(BuildJobType::BUILD_JOB_RERUN_CMAKE,
            InvokeAsyncCMakeCommand(BuiltInConfig::GetBuildDirectory()));
    }

    /// Invokes a new build of the current active module job
    void DoCompileCurrentProcessedModule()
    {
        ASSERT(_build_job, "There isn't any active build job!");

        TC_LOG_INFO("scripts.hotswap", "Starting asynchronous build job for module {}...",
                    _build_job->GetModuleName());

        _build_job->UpdateCurrentJob(BuildJobType::BUILD_JOB_COMPILE,
            InvokeAsyncCMakeCommand(
                "--build", BuiltInConfig::GetBuildDirectory(),
                "--target", _build_job->GetProjectName(),
                "--config", _build_job->GetBuildDirective()));
    }

    /// Invokes a new asynchronous install of the current active module job
    void DoInstallCurrentProcessedModule()
    {
        ASSERT(_build_job, "There isn't any active build job!");

        TC_LOG_INFO("scripts.hotswap", "Starting asynchronous install job for module {}...",
                    _build_job->GetModuleName());

        _build_job->UpdateCurrentJob(BuildJobType::BUILD_JOB_INSTALL,
            InvokeAsyncCMakeCommand(
                "-DCOMPONENT=" + _build_job->GetProjectName(),
                "-DBUILD_TYPE=" + _build_job->GetBuildDirective(),
                "-P", fs::absolute("cmake_install.cmake",
                    BuiltInConfig::GetBuildDirectory()).generic_string()));
    }

    /// Sets the CMAKE_INSTALL_PREFIX variable in the CMake cache
    /// to point to the current worldserver position,
    /// since most users will forget this.
    void DoCMakePrefixCorrectionIfNeeded()
    {
        TC_LOG_INFO("scripts.hotswap", "Correcting your CMAKE_INSTALL_PREFIX in \"{}\"...",
                    BuiltInConfig::GetBuildDirectory());

        auto const cmake_cache_path = fs::absolute("CMakeCache.txt",
            BuiltInConfig::GetBuildDirectory());

        // Stop when the CMakeCache wasn't found
        if (![&]
        {
            boost::system::error_code error;
            if (!fs::exists(cmake_cache_path, error))
            {
                TC_LOG_ERROR("scripts.hotswap", ">> CMake cache \"{}\" doesn't exist, "
                    "set the \"BuildDirectory\" option in your worldserver.conf to point"
                    "to your build directory!",
                    cmake_cache_path.generic_string());

                return false;
            }
            else
                return true;
        }())
            return;

        TC_LOG_TRACE("scripts.hotswap", "Checking CMake cache (\"{}\") "
                     "for the correct CMAKE_INSTALL_PREFIX location...",
                     cmake_cache_path.generic_string());

        std::string cmake_cache_content;
        {
            std::ifstream in(cmake_cache_path.generic_string());
            if (!in.is_open())
            {
                TC_LOG_ERROR("scripts.hotswap", ">> Failed to read the CMake cache at \"{}\"!",
                    cmake_cache_path.generic_string());

                return;
            }

            std::ostringstream ss;
            ss << in.rdbuf();
            cmake_cache_content = ss.str();

            in.close();
        }

        static std::string const prefix_key = "CMAKE_INSTALL_PREFIX:PATH=";

        // Extract the value of CMAKE_INSTALL_PREFIX
        auto begin = cmake_cache_content.find(prefix_key);
        if (begin != std::string::npos)
        {
            begin += prefix_key.length();
            auto const end = cmake_cache_content.find("\n", begin);
            if (end != std::string::npos)
            {
                fs::path value = cmake_cache_content.substr(begin, end - begin);

                auto current_path = fs::current_path();

            #if TRINITY_PLATFORM != TRINITY_PLATFORM_WINDOWS
                // The worldserver location is ${CMAKE_INSTALL_PREFIX}/bin
                // on all other platforms then windows
                current_path = current_path.parent_path();
            #endif

                if (value != current_path)
                {
                    // Prevent correction of the install prefix
                    // when we are starting the core from inside the build tree
                    bool const is_in_path = [&]
                    {
                        fs::path base = BuiltInConfig::GetBuildDirectory();
                        fs::path branch = value;
                        while (!branch.empty())
                        {
                            if (base == branch)
                                return true;

                            branch = branch.parent_path();
                        }

                        return false;
                    }();

                    if (is_in_path)
                        return;

                    TC_LOG_INFO("scripts.hotswap", ">> Found outdated CMAKE_INSTALL_PREFIX (\"{}\"), "
                        "worldserver is currently installed at {}",
                        value.generic_string(), current_path.generic_string());
                }
                else
                {
                    TC_LOG_INFO("scripts.hotswap", ">> CMAKE_INSTALL_PREFIX is equal to the current path of execution.");
                    return;
                }
            }
        }

        TC_LOG_INFO("scripts.hotswap", "Invoking CMake cache correction...");

        auto const error = InvokeCMakeCommand(
            "-DCMAKE_INSTALL_PREFIX:PATH=" + fs::current_path().generic_string(),
            BuiltInConfig::GetBuildDirectory());

        if (error)
        {
            TC_LOG_ERROR("scripts.hotswap", ">> Failed to update the CMAKE_INSTALL_PREFIX! "
                "This could lead to unexpected behaviour!");
        }
        else
        {
            TC_LOG_ERROR("scripts.hotswap", ">> Successfully corrected your CMAKE_INSTALL_PREFIX variable"
                         "to point at your current path of execution.");
        }
    }

    // File watcher instance and watcher ID's
    efsw::FileWatcher _fileWatcher;
    efsw::WatchID _libraryWatcher;

    // Unique library name counter which is used to
    // generate unique names for every shared library version.
    uint32 _unique_library_name_counter;

    // Queue which is used for thread safe message processing
    MPSCQueue<ScriptReloaderMessage> _messages;

    // Change requests to load or unload shared libraries
    std::unordered_set<fs::path /*path*/> _libraries_changed;
    // The timestamp which indicates the last time a library was changed
    uint32 _last_time_library_changed;

    // Contains all running script modules
    // The associated shared libraries are unloaded immediately
    // on loosing ownership through RAII.
    std::unordered_map<std::string /*module name*/,
        std::pair<std::shared_ptr<ScriptModule>, std::unique_ptr<SourceUpdateListener>>
    > _running_script_modules;
    // Container which maps the path of a shared library to it's module name
    std::unordered_map<fs::path, std::string /*module name*/>  _running_script_module_names;
    // Container which maps the module name to it's last known build directive
    std::unordered_map<std::string /*module name*/, std::string /*build directive*/> _known_modules_build_directives;

    // Modules which were changed and are queued for recompilation
    std::unordered_map<std::string /*module*/,
        std::unordered_map<fs::path /*path*/, ChangeStateRequest /*state*/>> _sources_changed;
    // Tracks the time since the last module has changed to avoid burst updates
    uint32 _last_time_sources_changed;

    // Tracks the last timestamp the user was informed about a certain repeating event.
    uint32 _last_time_user_informed;

    // Represents the current build job which is in progress
    Optional<BuildJob> _build_job;

    // Is true when the build job dispatcher should stop after
    // the current job has finished
    bool terminate_early;

    // The path to the tc_scripts temporary cache
    fs::path temporary_cache_path_;
};

class ScriptModuleDeleteMessage
{
public:
    explicit ScriptModuleDeleteMessage(ScriptModule* module)
        : module_(module) { }

    void operator() (HotSwapScriptReloadMgr*)
    {
        module_.reset();
    }

private:
    std::unique_ptr<ScriptModule> module_;
};

void ScriptModule::ScheduleDelayedDelete(ScriptModule* module)
{
    sScriptReloadMgr->QueueMessage(ScriptModuleDeleteMessage(module));
}

/// Maps efsw actions to strings
static char const* ActionToString(efsw::Action action)
{
    switch (action)
    {
        case efsw::Action::Add:
            return "added";
        case efsw::Action::Delete:
            return "deleted";
        case efsw::Action::Moved:
            return "moved";
        default:
            return "modified";
    }
}

void LibraryUpdateListener::handleFileAction(efsw::WatchID watchid, std::string const& dir,
    std::string const& filename, efsw::Action action, std::string oldFilename)
{
    // TC_LOG_TRACE("scripts.hotswap", "Library listener detected change on possible module \"{}\ ({})".", filename, ActionToString(action));

    // Split moved actions into a delete and an add action
    if (action == efsw::Action::Moved)
    {
        ASSERT(!oldFilename.empty(), "Old filename doesn't exist!");
        handleFileAction(watchid, dir, oldFilename, efsw::Action::Delete);
        handleFileAction(watchid, dir, filename, efsw::Action::Add);
        return;
    }

    sScriptReloadMgr->QueueMessage([=](HotSwapScriptReloadMgr* reloader) mutable
    {
        auto const path = fs::absolute(
            filename,
            sScriptReloadMgr->GetLibraryDirectory());

        if (!HasValidScriptModuleName(filename))
            return;

        switch (action)
        {
            case efsw::Actions::Add:
                TC_LOG_TRACE("scripts.hotswap", ">> Loading \"{}\" ({})...",
                    path.generic_string(), ActionToString(action));
                reloader->QueueSharedLibraryChanged(path);
                break;
            case efsw::Actions::Delete:
                TC_LOG_TRACE("scripts.hotswap", ">> Unloading \"{}\" ({})...",
                    path.generic_string(), ActionToString(action));
                reloader->QueueSharedLibraryChanged(path);
                break;
            case efsw::Actions::Modified:
                TC_LOG_TRACE("scripts.hotswap", ">> Reloading \"{}\" ({})...",
                    path.generic_string(), ActionToString(action));
                reloader->QueueSharedLibraryChanged(path);
                break;
            default:
                ABORT();
                break;
        }
    });
}

/// Returns true when the given path has a known C++ file extension
static bool HasCXXSourceFileExtension(fs::path const& path)
{
    static Trinity::regex const regex("^\\.(h|hpp|c|cc|cpp)$");
    return Trinity::regex_match(path.extension().generic_string(), regex);
}

SourceUpdateListener::SourceUpdateListener(fs::path path, std::string script_module_name)
    : path_(std::move(path)), script_module_name_(std::move(script_module_name)),
      watcher_id_(sScriptReloadMgr->_fileWatcher.addWatch(path_.generic_string(), this, true))
{
    if (watcher_id_ >= 0)
    {
        TC_LOG_TRACE("scripts.hotswap", ">> Attached the source recompiler to \"{}\".",
            path_.generic_string());
    }
    else
    {
        TC_LOG_ERROR("scripts.hotswap", "Failed to initialize thesource recompiler on \"{}\".",
            path_.generic_string());
    }
}

SourceUpdateListener::~SourceUpdateListener()
{
    if (watcher_id_ >= 0)
    {
        sScriptReloadMgr->_fileWatcher.removeWatch(watcher_id_);

        TC_LOG_TRACE("scripts.hotswap", ">> Detached the source recompiler from \"{}\".",
            path_.generic_string());
    }
}

void SourceUpdateListener::handleFileAction(efsw::WatchID watchid, std::string const& dir,
    std::string const& filename, efsw::Action action, std::string oldFilename)
{
    // TC_LOG_TRACE("scripts.hotswap", "Source listener detected change on possible file \"{}/{}\" ({}).", dir, filename, ActionToString(action));

    // Skip the file change notification if the recompiler is disabled
    if (!sWorld->getBoolConfig(CONFIG_HOTSWAP_RECOMPILER_ENABLED))
        return;

    // Split moved actions into a delete and an add action
    if (action == efsw::Action::Moved)
    {
        ASSERT(!oldFilename.empty(), "Old filename doesn't exist!");
        handleFileAction(watchid, dir, oldFilename, efsw::Action::Delete);
        handleFileAction(watchid, dir, filename, efsw::Action::Add);
        return;
    }

    fs::path path = fs::absolute(
        filename,
        dir);

    // Check if the file is a C/C++ source file.
    if (!path.has_extension() || !HasCXXSourceFileExtension(path))
        return;

    /// Thread safe part
    sScriptReloadMgr->QueueMessage([=, this, path = std::move(path)](HotSwapScriptReloadMgr* reloader)
    {
        TC_LOG_TRACE("scripts.hotswap", "Detected source change on module \"{}\", "
            "queued for recompilation...", script_module_name_);

        switch (action)
        {
            case efsw::Actions::Add:
                TC_LOG_TRACE("scripts.hotswap", "Source file {} of module {} was added.",
                    path.generic_string(), script_module_name_);
                reloader->QueueAddSourceFile(script_module_name_, path);
                break;
            case efsw::Actions::Delete:
                TC_LOG_TRACE("scripts.hotswap", "Source file {} of module {} was deleted.",
                    path.generic_string(), script_module_name_);
                reloader->QueueRemoveSourceFile(script_module_name_, path);
                break;
            case efsw::Actions::Modified:
                TC_LOG_TRACE("scripts.hotswap", "Source file {} of module {} was modified.",
                    path.generic_string(), script_module_name_);
                reloader->QueueModifySourceFile(script_module_name_, path);
                break;
            default:
                ABORT();
                break;
        }
    });
}

// Returns the module reference of the given context
std::shared_ptr<ModuleReference>
    ScriptReloadMgr::AcquireModuleReferenceOfContext(std::string const& context)
{
    // Return empty references for the static context exported by the worldserver
    if (context == ScriptMgr::GetNameOfStaticContext())
        return { };

    auto const itr = sScriptReloadMgr->_running_script_modules.find(context);
    ASSERT(itr != sScriptReloadMgr->_running_script_modules.end()
           && "Requested a reference to a non existent script context!");

    return itr->second.first;
}

// Returns the full hot swap implemented ScriptReloadMgr
ScriptReloadMgr* ScriptReloadMgr::instance()
{
    static HotSwapScriptReloadMgr instance;
    return &instance;
}

#endif // #ifndef TRINITY_API_USE_DYNAMIC_LINKING
