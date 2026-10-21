#include "core/task.h"

#include <windows.h>

#define SECURITY_WIN32
#include <secext.h>
#include <taskschd.h>

#include <format>
#include <string>

#include "core/log.h"
#include "core/util.h"

namespace grd {
namespace {

constexpr wchar_t kTaskName[] = L"GpuRamDriveVk";

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() {
        if (ptr_ != nullptr) {
            ptr_->Release();
        }
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put() {
        reset();
        return &ptr_;
    }
    T* operator->() const { return ptr_; }
    T* get() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset() {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

private:
    T* ptr_ = nullptr;
};

class ComInitializer {
public:
    ComInitializer() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = SUCCEEDED(hr);
        initialized_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
    ~ComInitializer() {
        if (owned_) {
            CoUninitialize();
        }
    }
    ComInitializer(const ComInitializer&) = delete;
    ComInitializer& operator=(const ComInitializer&) = delete;

    bool initialized() const { return initialized_; }

private:
    bool owned_ = false;
    bool initialized_ = false;
};

std::wstring hresultText(HRESULT hr) {
    return std::format(L"0x{:08X}", static_cast<unsigned long>(hr));
}

VARIANT emptyVariant() {
    VARIANT value;
    VariantInit(&value);
    return value;
}

VARIANT stringVariant(const std::wstring& text) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(text.c_str());
    return value;
}

bool failed(HRESULT hr, const wchar_t* operation, std::wstring& error) {
    if (SUCCEEDED(hr)) {
        return false;
    }
    error = std::wstring(operation) + L" failed (" + hresultText(hr) + L").";
    return true;
}

std::wstring taskExecutable() {
    const std::wstring gui = joinPath(executableDir(), L"gpuvramdrive.exe");
    if (fileExists(gui)) {
        return gui;
    }
    return modulePath();
}

std::wstring currentUserName() {
    ULONG size = 0;
    GetUserNameExW(NameSamCompatible, nullptr, &size);
    if (size > 0) {
        std::wstring buffer(size, L'\0');
        if (GetUserNameExW(NameSamCompatible, buffer.data(), &size) != FALSE) {
            buffer.resize(size);
            while (!buffer.empty() && buffer.back() == L'\0') {
                buffer.pop_back();
            }
            if (!buffer.empty()) {
                return buffer;
            }
        }
    }

    std::wstring buffer(256, L'\0');
    DWORD length = static_cast<DWORD>(buffer.size());
    if (GetUserNameW(buffer.data(), &length) == FALSE) {
        return {};
    }
    buffer.resize(length > 0 ? length - 1 : 0);
    return buffer;
}

bool connect(ComPtr<ITaskService>& service, std::wstring& error) {
    if (failed(CoCreateInstance(CLSID_TaskScheduler, nullptr,
                                CLSCTX_INPROC_SERVER, IID_ITaskService,
                                reinterpret_cast<void**>(service.put())),
               L"CoCreateInstance(TaskScheduler)", error)) {
        return false;
    }

    VARIANT empty = emptyVariant();
    const HRESULT hr = service->Connect(empty, empty, empty, empty);
    VariantClear(&empty);
    return !failed(hr, L"ITaskService::Connect", error);
}

bool openRoot(ComPtr<ITaskService>& service, ComPtr<ITaskFolder>& folder,
              std::wstring& error) {
    BSTR path = SysAllocString(L"\\");
    const HRESULT hr = service->GetFolder(path, folder.put());
    SysFreeString(path);
    return !failed(hr, L"ITaskService::GetFolder", error);
}

bool registerTask(std::wstring& error) {
    ComPtr<ITaskService> service;
    if (!connect(service, error)) {
        return false;
    }

    ComPtr<ITaskFolder> folder;
    if (!openRoot(service, folder, error)) {
        return false;
    }

    ComPtr<ITaskDefinition> definition;
    if (failed(service->NewTask(0, definition.put()), L"ITaskService::NewTask",
               error)) {
        return false;
    }

    ComPtr<IRegistrationInfo> registration;
    if (failed(definition->get_RegistrationInfo(registration.put()),
               L"ITaskDefinition::get_RegistrationInfo", error)) {
        return false;
    }
    BSTR author = SysAllocString(L"GpuRamDriveVk");
    registration->put_Author(author);
    SysFreeString(author);

    ComPtr<IPrincipal> principal;
    if (failed(definition->get_Principal(principal.put()),
               L"ITaskDefinition::get_Principal", error)) {
        return false;
    }
    principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
    principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);

    ComPtr<ITaskSettings> settings;
    if (failed(definition->get_Settings(settings.put()),
               L"ITaskDefinition::get_Settings", error)) {
        return false;
    }
    settings->put_StartWhenAvailable(VARIANT_TRUE);
    settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
    settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    settings->put_Enabled(VARIANT_TRUE);
    settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
    BSTR unlimited = SysAllocString(L"PT0S");
    settings->put_ExecutionTimeLimit(unlimited);
    SysFreeString(unlimited);

    ComPtr<ITriggerCollection> triggers;
    if (failed(definition->get_Triggers(triggers.put()),
               L"ITaskDefinition::get_Triggers", error)) {
        return false;
    }

    ComPtr<ITrigger> trigger;
    if (failed(triggers->Create(TASK_TRIGGER_LOGON, trigger.put()),
               L"ITriggerCollection::Create", error)) {
        return false;
    }

    ComPtr<ILogonTrigger> logonTrigger;
    if (failed(trigger->QueryInterface(
                   IID_ILogonTrigger,
                   reinterpret_cast<void**>(logonTrigger.put())),
               L"ITrigger::QueryInterface(ILogonTrigger)", error)) {
        return false;
    }
    BSTR triggerId = SysAllocString(L"Logon");
    logonTrigger->put_Id(triggerId);
    SysFreeString(triggerId);
    logonTrigger->put_Enabled(VARIANT_TRUE);
    const std::wstring user = currentUserName();
    if (!user.empty()) {
        BSTR userId = SysAllocString(user.c_str());
        logonTrigger->put_UserId(userId);
        SysFreeString(userId);
    }

    ComPtr<IActionCollection> actions;
    if (failed(definition->get_Actions(actions.put()),
               L"ITaskDefinition::get_Actions", error)) {
        return false;
    }

    ComPtr<IAction> action;
    if (failed(actions->Create(TASK_ACTION_EXEC, action.put()),
               L"IActionCollection::Create", error)) {
        return false;
    }

    ComPtr<IExecAction> exec;
    if (failed(action->QueryInterface(IID_IExecAction,
                                      reinterpret_cast<void**>(exec.put())),
               L"IAction::QueryInterface(IExecAction)", error)) {
        return false;
    }

    BSTR executable = SysAllocString(taskExecutable().c_str());
    BSTR arguments = SysAllocString(L"--autostart");
    BSTR workingDirectory = SysAllocString(executableDir().c_str());
    exec->put_Path(executable);
    exec->put_Arguments(arguments);
    exec->put_WorkingDirectory(workingDirectory);
    SysFreeString(executable);
    SysFreeString(arguments);
    SysFreeString(workingDirectory);

    BSTR name = SysAllocString(kTaskName);
    VARIANT userId = user.empty() ? emptyVariant() : stringVariant(user);
    VARIANT password = emptyVariant();
    VARIANT sddl = emptyVariant();
    ComPtr<IRegisteredTask> registered;
    const HRESULT hr = folder->RegisterTaskDefinition(
        name, definition.get(), TASK_CREATE_OR_UPDATE, userId, password,
        TASK_LOGON_INTERACTIVE_TOKEN, sddl, registered.put());
    VariantClear(&userId);
    VariantClear(&password);
    VariantClear(&sddl);
    SysFreeString(name);

    return !failed(hr, L"ITaskFolder::RegisterTaskDefinition", error);
}

bool deleteTask(std::wstring& error) {
    ComPtr<ITaskService> service;
    if (!connect(service, error)) {
        return false;
    }

    ComPtr<ITaskFolder> folder;
    if (!openRoot(service, folder, error)) {
        return false;
    }

    BSTR name = SysAllocString(kTaskName);
    const HRESULT hr = folder->DeleteTask(name, 0);
    SysFreeString(name);

    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        return true;
    }
    return !failed(hr, L"ITaskFolder::DeleteTask", error);
}

}  // namespace

bool autoStartTaskExists() {
    ComInitializer com;
    if (!com.initialized()) {
        return false;
    }

    std::wstring error;
    ComPtr<ITaskService> service;
    if (!connect(service, error)) {
        return false;
    }

    ComPtr<ITaskFolder> folder;
    if (!openRoot(service, folder, error)) {
        return false;
    }

    BSTR name = SysAllocString(kTaskName);
    ComPtr<IRegisteredTask> task;
    const HRESULT hr = folder->GetTask(name, task.put());
    SysFreeString(name);
    return SUCCEEDED(hr);
}

bool setAutoStartTask(bool enabled, std::wstring& error) {
    error.clear();
    ComInitializer com;
    if (!com.initialized()) {
        error = L"Unable to initialize COM.";
        return false;
    }

    const bool ok = enabled ? registerTask(error) : deleteTask(error);
    if (ok) {
        Log::info(enabled ? L"Enabled autostart." : L"Disabled autostart.");
    }
    return ok;
}

}  // namespace grd
