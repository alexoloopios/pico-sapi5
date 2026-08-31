// DLL entry points and the class factory.
//
// The whole engine is one in-process COM class. SAPI creates it, hands it the
// voice token that says which of the six voices it stands for, and then calls
// Speak; there is nothing else to expose.

#include <windows.h>

#include <new>

#include "pico_log.hpp"
#include "pico_registry.hpp"
#include "pico_tts_engine.hpp"

namespace {

HMODULE g_module = nullptr;
LONG g_objectCount = 0;

void ObjectCreated() {
	InterlockedIncrement(&g_objectCount);
}

void ObjectDestroyed() {
	InterlockedDecrement(&g_objectCount);
}

//: Tracks the engine objects so DllCanUnloadNow can answer honestly. The engine
//: itself does not know about the count, so it is wrapped here.
class CountedEngine : public pico::PicoTTSEngine {
public:
	CountedEngine() { ObjectCreated(); }
	~CountedEngine() override { ObjectDestroyed(); }
};

class ClassFactory : public IClassFactory {
public:
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
		if (ppv == nullptr) {
			return E_POINTER;
		}
		if (riid == IID_IUnknown || riid == IID_IClassFactory) {
			*ppv = static_cast<IClassFactory*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	STDMETHODIMP_(ULONG) AddRef() override {
		return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
	}

	STDMETHODIMP_(ULONG) Release() override {
		const LONG remaining = InterlockedDecrement(&referenceCount_);
		if (remaining == 0) {
			delete this;
			return 0;
		}
		return static_cast<ULONG>(remaining);
	}

	STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
		if (ppv == nullptr) {
			return E_POINTER;
		}
		*ppv = nullptr;
		if (pUnkOuter != nullptr) {
			// Aggregation would mean sharing an IUnknown with an outer object,
			// which this engine has no way to support.
			return CLASS_E_NOAGGREGATION;
		}
		CountedEngine* engine = new (std::nothrow) CountedEngine();
		if (engine == nullptr) {
			return E_OUTOFMEMORY;
		}
		const HRESULT hr = engine->QueryInterface(riid, ppv);
		// The object is created with one reference; QueryInterface took its own,
		// so this releases the one the constructor left behind.
		engine->Release();
		return hr;
	}

	STDMETHODIMP LockServer(BOOL fLock) override {
		if (fLock) {
			ObjectCreated();
		} else {
			ObjectDestroyed();
		}
		return S_OK;
	}

private:
	LONG referenceCount_ = 1;
};

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID /*reserved*/) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_module = instance;
		// The engine has no per-thread state, and the notifications cost every
		// thread the host creates.
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
	if (ppv == nullptr) {
		return E_POINTER;
	}
	*ppv = nullptr;
	if (rclsid != __uuidof(pico::PicoTTSEngine)) {
		return CLASS_E_CLASSNOTAVAILABLE;
	}
	ClassFactory* factory = new (std::nothrow) ClassFactory();
	if (factory == nullptr) {
		return E_OUTOFMEMORY;
	}
	const HRESULT hr = factory->QueryInterface(riid, ppv);
	factory->Release();
	return hr;
}

STDAPI DllCanUnloadNow() {
	return g_objectCount == 0 ? S_OK : S_FALSE;
}

// Registration is machine wide and needs elevation. There is deliberately no
// DllInstall alongside these for a per-user install: SAPI enumerates a voice
// category only from the hive SPCAT_VOICES names, which is HKEY_LOCAL_MACHINE,
// so tokens written under HKEY_CURRENT_USER are never found.

STDAPI DllRegisterServer() {
	return pico::RegisterServer();
}

STDAPI DllUnregisterServer() {
	return pico::UnregisterServer();
}
