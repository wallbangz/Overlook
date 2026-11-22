// UI Automation for the settings window: its drawn controls are exposed as UIA elements (toggle
// switches, sliders, combo boxes, edit boxes, buttons, links, page tabs), so Narrator and other
// screen readers can read and change them. The providers live on the window's thread
// (ProviderOptions_UseComThreading) and read the window's state directly.
#include "settingsui.h"
#include <UIAutomation.h>
#include <utility>

namespace sw {

class Root;
class Element;

// Shared between the window and every provider handed out; the window detaches it on destroy,
// after which providers answer UIA_E_ELEMENTNOTAVAILABLE.
class UiaLink {
public:
    Window* w = nullptr;
    LONG refs = 1;
    Root* root = nullptr;   // owned (one reference)
    std::vector<std::pair<int, Element*>> cache;   // by control id, one reference each
    void AddRef() { InterlockedIncrement(&refs); }
    void Release() { if (InterlockedDecrement(&refs) == 0) delete this; }
};

namespace {

BSTR Bstr(const std::wstring& s) { return SysAllocStringLen(s.c_str(), (UINT)s.size()); }

}   // namespace

class Element : public IRawElementProviderSimple, public IRawElementProviderFragment, public IToggleProvider,
                public IRangeValueProvider, public IValueProvider, public IInvokeProvider,
                public IExpandCollapseProvider, public ISelectionItemProvider {
public:
    Element(UiaLink* link, int id) : link_(link), id_(id) { link_->AddRef(); }
    virtual ~Element() { link_->Release(); }

    // The control, or -1 once it (or the window) is gone.
    int Index() const {
        if (!link_->w) return -1;
        const int i = link_->w->CtlIndex(id_);
        return i >= 0 && link_->w->ctls[i].visible ? i : -1;
    }
    Kind KindOf() const { const int i = Index(); return i >= 0 ? link_->w->ctls[i].kind : Kind::Button; }
    bool Has(PATTERNID p) const {
        switch (KindOf()) {
        case Kind::NavItem: return p == UIA_SelectionItemPatternId;
        case Kind::Combo: return p == UIA_ExpandCollapsePatternId || p == UIA_ValuePatternId;
        case Kind::EditCombo: return p == UIA_ExpandCollapsePatternId || p == UIA_ValuePatternId;
        case Kind::Toggle: return p == UIA_TogglePatternId;
        case Kind::Slider: return p == UIA_RangeValuePatternId;
        case Kind::Edit: return p == UIA_ValuePatternId;
        case Kind::Color: case Kind::Button: case Kind::Link: case Kind::Reset: return p == UIA_InvokePatternId;
        }
        return false;
    }

    // IUnknown
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
        else if (iid == __uuidof(IToggleProvider) && Has(UIA_TogglePatternId)) *out = static_cast<IToggleProvider*>(this);
        else if (iid == __uuidof(IRangeValueProvider) && Has(UIA_RangeValuePatternId)) *out = static_cast<IRangeValueProvider*>(this);
        else if (iid == __uuidof(IValueProvider) && Has(UIA_ValuePatternId)) *out = static_cast<IValueProvider*>(this);
        else if (iid == __uuidof(IInvokeProvider) && Has(UIA_InvokePatternId)) *out = static_cast<IInvokeProvider*>(this);
        else if (iid == __uuidof(IExpandCollapseProvider) && Has(UIA_ExpandCollapsePatternId)) *out = static_cast<IExpandCollapseProvider*>(this);
        else if (iid == __uuidof(ISelectionItemProvider) && Has(UIA_SelectionItemPatternId)) *out = static_cast<ISelectionItemProvider*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    // IRawElementProviderSimple
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* r) override {
        *r = (ProviderOptions)(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID p, IUnknown** r) override {
        *r = nullptr;
        if (Index() < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        if (Has(p)) { *r = static_cast<IRawElementProviderSimple*>(this); AddRef(); }
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID p, VARIANT* v) override;
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** r) override { *r = nullptr; return S_OK; }

    // IRawElementProviderFragment
    IFACEMETHODIMP Navigate(NavigateDirection dir, IRawElementProviderFragment** r) override;
    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** r) override {
        *r = nullptr;
        if (Index() < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        int ids[2] = { UiaAppendRuntimeId, id_ };
        *r = SafeArrayCreateVector(VT_I4, 0, 2);
        if (!*r) return E_OUTOFMEMORY;
        for (LONG k = 0; k < 2; ++k) SafeArrayPutElement(*r, &k, &ids[k]);
        return S_OK;
    }
    IFACEMETHODIMP get_BoundingRectangle(UiaRect* r) override {
        *r = {};
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        if (link_->w->Offscreen(i)) return S_OK;
        const RECT rc = link_->w->ScreenRect(i);
        *r = { (double)rc.left, (double)rc.top, (double)(rc.right - rc.left), (double)(rc.bottom - rc.top) };
        return S_OK;
    }
    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** r) override { *r = nullptr; return S_OK; }
    IFACEMETHODIMP SetFocus() override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        link_->w->UiaFocus(i);
        return S_OK;
    }
    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** r) override;

    // IToggleProvider
    IFACEMETHODIMP Toggle() override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!link_->w->ctls[i].enabled) return UIA_E_ELEMENTNOTENABLED;
        link_->w->UiaToggle(i);
        return S_OK;
    }
    IFACEMETHODIMP get_ToggleState(ToggleState* r) override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        *r = link_->w->ToggleOn(i) ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

    // IRangeValueProvider
    IFACEMETHODIMP SetValue(double v) override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        const Ctl& c = link_->w->ctls[i];
        if (!c.enabled) return UIA_E_ELEMENTNOTENABLED;
        if (v < c.lo || v > c.hi) return E_INVALIDARG;
        link_->w->UiaSetRange(i, v);
        return S_OK;
    }
    IFACEMETHODIMP get_Value(double* r) override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        *r = link_->w->SliderValue(i);
        return S_OK;
    }
    IFACEMETHODIMP get_IsReadOnly(BOOL* r) override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        *r = !link_->w->ctls[i].enabled || link_->w->ctls[i].kind == Kind::Combo;
        return S_OK;
    }
    IFACEMETHODIMP get_Maximum(double* r) override { const int i = Index(); if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE; *r = link_->w->ctls[i].hi; return S_OK; }
    IFACEMETHODIMP get_Minimum(double* r) override { const int i = Index(); if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE; *r = link_->w->ctls[i].lo; return S_OK; }
    IFACEMETHODIMP get_LargeChange(double* r) override { const int i = Index(); if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE; *r = link_->w->ctls[i].big; return S_OK; }
    IFACEMETHODIMP get_SmallChange(double* r) override { const int i = Index(); if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE; *r = link_->w->ctls[i].step; return S_OK; }

    // IValueProvider (IsReadOnly is shared with IRangeValueProvider)
    IFACEMETHODIMP SetValue(LPCWSTR v) override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!link_->w->ctls[i].enabled) return UIA_E_ELEMENTNOTENABLED;
        return link_->w->UiaSetValue(i, v ? v : L"") ? S_OK : E_INVALIDARG;
    }
    IFACEMETHODIMP get_Value(BSTR* r) override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        *r = Bstr(link_->w->ValueText(i));
        return S_OK;
    }

    // IInvokeProvider
    IFACEMETHODIMP Invoke() override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!link_->w->ctls[i].enabled) return UIA_E_ELEMENTNOTENABLED;
        // The colour picker is modal: run it after this call returns, not inside it.
        HWND h = link_->w->hwnd;
        PostMessageW(h, WM_APP + 41, (WPARAM)id_, 0);
        return S_OK;
    }

    // IExpandCollapseProvider
    IFACEMETHODIMP Expand() override { const int i = Index(); if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE; link_->w->UiaExpand(i, true); return S_OK; }
    IFACEMETHODIMP Collapse() override { const int i = Index(); if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE; link_->w->UiaExpand(i, false); return S_OK; }
    IFACEMETHODIMP get_ExpandCollapseState(ExpandCollapseState* r) override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        *r = link_->w->PopupOpenFor(i) ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
        return S_OK;
    }

    // ISelectionItemProvider (the page list)
    IFACEMETHODIMP Select() override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        link_->w->UiaFocus(i);
        return S_OK;
    }
    IFACEMETHODIMP AddToSelection() override { return Select(); }
    IFACEMETHODIMP RemoveFromSelection() override { return UIA_E_INVALIDOPERATION; }
    IFACEMETHODIMP get_IsSelected(BOOL* r) override {
        const int i = Index();
        if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        *r = link_->w->ctls[i].owner == link_->w->CurrentPage();
        return S_OK;
    }
    IFACEMETHODIMP get_SelectionContainer(IRawElementProviderSimple** r) override { *r = nullptr; return S_OK; }

    int id_;

private:
    UiaLink* link_;
    LONG refs_ = 1;
};

class Root : public IRawElementProviderSimple, public IRawElementProviderFragment, public IRawElementProviderFragmentRoot {
public:
    explicit Root(UiaLink* link) : link_(link) { link_->AddRef(); }   // the link drops its reference on detach
    virtual ~Root() { link_->Release(); }

    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple)) *out = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == __uuidof(IRawElementProviderFragment)) *out = static_cast<IRawElementProviderFragment*>(this);
        else if (iid == __uuidof(IRawElementProviderFragmentRoot)) *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* r) override {
        *r = (ProviderOptions)(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID, IUnknown** r) override { *r = nullptr; return S_OK; }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID p, VARIANT* v) override {
        v->vt = VT_EMPTY;
        if (p == UIA_AutomationIdPropertyId) { v->vt = VT_BSTR; v->bstrVal = SysAllocString(L"OverlookSettings"); }
        return S_OK;
    }
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** r) override {
        *r = nullptr;
        if (!link_->w) return UIA_E_ELEMENTNOTAVAILABLE;
        return UiaHostProviderFromHwnd(link_->w->hwnd, r);
    }
    IFACEMETHODIMP Navigate(NavigateDirection dir, IRawElementProviderFragment** r) override;
    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** r) override { *r = nullptr; return S_OK; }
    IFACEMETHODIMP get_BoundingRectangle(UiaRect* r) override { *r = {}; return S_OK; }
    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** r) override { *r = nullptr; return S_OK; }
    IFACEMETHODIMP SetFocus() override { return S_OK; }
    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** r) override { *r = this; AddRef(); return S_OK; }
    IFACEMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** r) override;
    IFACEMETHODIMP GetFocus(IRawElementProviderFragment** r) override;

private:
    UiaLink* link_;
    LONG refs_ = 1;
};

// The element for control i, created once and reused (one reference returned to the caller).
static Element* ElementFor(UiaLink* link, int i) {
    if (!link->w || i < 0) return nullptr;
    const int id = link->w->ctls[i].id;
    for (auto& e : link->cache) if (e.first == id) { e.second->AddRef(); return e.second; }
    Element* e = new Element(link, id);
    link->cache.push_back({ id, e });
    e->AddRef();
    return e;
}

IFACEMETHODIMP Element::GetPropertyValue(PROPERTYID p, VARIANT* v) {
    v->vt = VT_EMPTY;
    const int i = Index();
    if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
    Window& w = *link_->w;
    const Ctl& c = w.ctls[i];
    auto str = [&](const std::wstring& s) { v->vt = VT_BSTR; v->bstrVal = Bstr(s); };
    auto boolean = [&](bool b) { v->vt = VT_BOOL; v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE; };
    switch (p) {
    case UIA_NamePropertyId: str(w.UiaName(i)); break;
    case UIA_HelpTextPropertyId: {
        std::wstring d = w.ShownDesc(i);
        if (!c.elsewhere.empty()) d += (d.empty() ? L"" : L". ") + c.elsewhere;
        if (!d.empty()) str(d);
        break;
    }
    case UIA_AutomationIdPropertyId:
        str(!c.monitorId.empty() ? L"Show:" + c.monitorId : c.kind == Kind::Reset ? L"Reset:" + w.ctls[c.owner].key
            : !c.key.empty() ? c.key + (c.kind == Kind::Edit && c.key == L"Color" ? L"Hex" : L"") : c.label);
        break;
    case UIA_ControlTypePropertyId: {
        int t = UIA_ButtonControlTypeId;
        switch (c.kind) {
        case Kind::NavItem: t = UIA_TabItemControlTypeId; break;
        case Kind::Combo: case Kind::EditCombo: t = UIA_ComboBoxControlTypeId; break;
        case Kind::Slider: t = UIA_SliderControlTypeId; break;
        case Kind::Edit: t = UIA_EditControlTypeId; break;
        case Kind::Link: t = UIA_HyperlinkControlTypeId; break;
        default: break;
        }
        v->vt = VT_I4; v->lVal = t;
        break;
    }
    case UIA_LocalizedControlTypePropertyId:
        if (c.kind == Kind::Toggle) str(L"toggle switch");
        else if (c.kind == Kind::NavItem) str(L"page");
        break;
    case UIA_IsEnabledPropertyId: boolean(c.enabled); break;
    case UIA_IsKeyboardFocusablePropertyId: boolean(c.enabled); break;
    case UIA_HasKeyboardFocusPropertyId: boolean(w.FocusIndex() == i && ::GetFocus() == w.hwnd); break;
    case UIA_IsOffscreenPropertyId: boolean(w.Offscreen(i)); break;
    case UIA_IsControlElementPropertyId: case UIA_IsContentElementPropertyId: boolean(true); break;
    case UIA_ClassNamePropertyId: str(L"OverlookSettingsControl"); break;
    case UIA_FrameworkIdPropertyId: str(L"Overlook"); break;
    default: break;
    }
    return S_OK;
}

IFACEMETHODIMP Element::Navigate(NavigateDirection dir, IRawElementProviderFragment** r) {
    *r = nullptr;
    const int i = Index();
    if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
    if (dir == NavigateDirection_Parent) {
        if (link_->root) { *r = link_->root; link_->root->AddRef(); }
        return S_OK;
    }
    if (dir == NavigateDirection_NextSibling || dir == NavigateDirection_PreviousSibling) {
        const auto order = link_->w->UiaOrder();
        for (size_t k = 0; k < order.size(); ++k) {
            if (order[k] != i) continue;
            const ptrdiff_t n = (ptrdiff_t)k + (dir == NavigateDirection_NextSibling ? 1 : -1);
            if (n >= 0 && n < (ptrdiff_t)order.size()) *r = ElementFor(link_, order[n]);
            break;
        }
    }
    return S_OK;
}

IFACEMETHODIMP Element::get_FragmentRoot(IRawElementProviderFragmentRoot** r) {
    *r = nullptr;
    if (Index() < 0) return UIA_E_ELEMENTNOTAVAILABLE;
    if (link_->root) { *r = link_->root; link_->root->AddRef(); }
    return S_OK;
}

IFACEMETHODIMP Root::Navigate(NavigateDirection dir, IRawElementProviderFragment** r) {
    *r = nullptr;
    if (!link_->w) return UIA_E_ELEMENTNOTAVAILABLE;
    if (dir == NavigateDirection_FirstChild || dir == NavigateDirection_LastChild) {
        const auto order = link_->w->UiaOrder();
        if (!order.empty()) *r = ElementFor(link_, dir == NavigateDirection_FirstChild ? order.front() : order.back());
    }
    return S_OK;
}

IFACEMETHODIMP Root::ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** r) {
    *r = nullptr;
    if (!link_->w) return UIA_E_ELEMENTNOTAVAILABLE;
    const int i = link_->w->CtlAtScreen({ (LONG)x, (LONG)y });
    if (i >= 0) *r = ElementFor(link_, i);
    else { *r = this; AddRef(); }
    return S_OK;
}

IFACEMETHODIMP Root::GetFocus(IRawElementProviderFragment** r) {
    *r = nullptr;
    if (!link_->w) return UIA_E_ELEMENTNOTAVAILABLE;
    const int i = link_->w->FocusIndex();
    if (i >= 0 && ::GetFocus() == link_->w->hwnd) *r = ElementFor(link_, i);
    return S_OK;
}

// ------------------------------------------------------------------ the window's side

static UiaLink* Link(Window& w) {
    if (!w.uia) {
        w.uia = new UiaLink();
        w.uia->w = &w;
        w.uia->root = new Root(w.uia);
    }
    return w.uia;
}

LRESULT UiaGetObject(Window& w, WPARAM wp, LPARAM lp) {
    if ((LONG)lp == UiaRootObjectId) return UiaReturnRawElementProvider(w.hwnd, wp, lp, Link(w)->root);
    return DefWindowProcW(w.hwnd, WM_GETOBJECT, wp, lp);
}

void UiaRaiseFocus(Window& w, int i) {
    if (!w.uia || !UiaClientsAreListening() || i < 0) return;
    if (Element* e = ElementFor(w.uia, i)) { UiaRaiseAutomationEvent(e, UIA_AutomationFocusChangedEventId); e->Release(); }
}

void UiaRaiseToggle(Window& w, int i, bool on) {
    if (!w.uia || !UiaClientsAreListening() || i < 0) return;
    if (Element* e = ElementFor(w.uia, i)) {
        VARIANT o{}, n{};
        o.vt = n.vt = VT_I4;
        o.lVal = on ? ToggleState_Off : ToggleState_On;
        n.lVal = on ? ToggleState_On : ToggleState_Off;
        UiaRaiseAutomationPropertyChangedEvent(e, UIA_ToggleToggleStatePropertyId, o, n);
        e->Release();
    }
}

void UiaRaiseValue(Window& w, int i) {
    if (!w.uia || !UiaClientsAreListening() || i < 0 || i >= (int)w.ctls.size()) return;
    Element* e = ElementFor(w.uia, i);
    if (!e) return;
    const Ctl& c = w.ctls[i];
    VARIANT o{}, n{};
    if (c.kind == Kind::Slider) {
        n.vt = VT_R8; n.dblVal = w.SliderValue(i);
        UiaRaiseAutomationPropertyChangedEvent(e, UIA_RangeValueValuePropertyId, o, n);
    } else if (c.kind == Kind::Combo || c.kind == Kind::EditCombo || c.kind == Kind::Edit) {
        n.vt = VT_BSTR; n.bstrVal = Bstr(w.ValueText(i));
        UiaRaiseAutomationPropertyChangedEvent(e, UIA_ValueValuePropertyId, o, n);
        VariantClear(&n);
        if (c.kind != Kind::Edit) {
            VARIANT s{};
            s.vt = VT_I4;
            s.lVal = w.PopupOpenFor(i) ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
            UiaRaiseAutomationPropertyChangedEvent(e, UIA_ExpandCollapseExpandCollapseStatePropertyId, o, s);
        }
    }
    e->Release();
}

void UiaDetach(Window& w) {
    if (!w.uia) return;
    UiaLink* link = w.uia;
    w.uia = nullptr;
    UiaReturnRawElementProvider(w.hwnd, 0, 0, nullptr);   // UIA lets go of this window's providers
    for (auto& e : link->cache) { UiaDisconnectProvider(e.second); e.second->Release(); }
    link->cache.clear();
    if (link->root) { UiaDisconnectProvider(link->root); link->root->Release(); link->root = nullptr; }
    link->w = nullptr;
    link->Release();
}

}   // namespace sw
