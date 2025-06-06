/***************************************************************************
 *   Copyright (c) 2002 Jürgen Riegel <juergen.riegel@web.de>              *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/


#include "PreCompiled.h"

#include <QDir>
#include <QFileInfo>
#include <boost/algorithm/string/predicate.hpp>

#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Reader.h>
#include <Base/Writer.h>
#include <Base/Tools.h>

#include "PropertyLinks.h"
#include "Application.h"
#include "Document.h"
#include "DocumentObject.h"
#include "DocumentObjectPy.h"
#include "DocumentObserver.h"
#include "ObjectIdentifier.h"
#include "ElementNamingUtils.h"
#include "GeoFeature.h"


FC_LOG_LEVEL_INIT("PropertyLinks", true, true)

using namespace App;
using namespace Base;
using namespace std;
namespace sp = std::placeholders;

//**************************************************************************
//**************************************************************************
// PropertyLinkBase
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE_ABSTRACT(App::PropertyLinkBase, App::Property)

// clang-format off
static std::unordered_map<std::string, std::set<PropertyLinkBase*>> _LabelMap;
static std::unordered_map<App::DocumentObject*, std::unordered_set<PropertyLinkBase*>> _ElementRefMap;
// clang-format on

PropertyLinkBase::PropertyLinkBase() = default;

PropertyLinkBase::~PropertyLinkBase()
{
    unregisterLabelReferences();
    unregisterElementReference();
}

void PropertyLinkBase::setAllowExternal(bool allow)
{
    setFlag(LinkAllowExternal, allow);
}

void PropertyLinkBase::setSilentRestore(bool allow)
{
    setFlag(LinkSilentRestore, allow);
}

void PropertyLinkBase::setReturnNewElement(bool enable)
{
    setFlag(LinkNewElement, enable);
}

void PropertyLinkBase::hasSetValue()
{
    auto owner = dynamic_cast<DocumentObject*>(getContainer());
    if (owner) {
        owner->clearOutListCache();
    }
    Property::hasSetValue();
}

bool PropertyLinkBase::isSame(const Property& other) const
{
    if (&other == this) {
        return true;
    }
    if (other.isDerivedFrom<PropertyLinkBase>()
        || getScope() != static_cast<const PropertyLinkBase*>(&other)->getScope()) {
        return false;
    }

    static std::vector<App::DocumentObject*> ret;
    static std::vector<std::string> subs;
    static std::vector<App::DocumentObject*> ret2;
    static std::vector<std::string> subs2;

    ret.clear();
    subs.clear();
    ret2.clear();
    subs2.clear();
    getLinks(ret, true, &subs, false);
    static_cast<const PropertyLinkBase*>(&other)->getLinks(ret2, true, &subs2, true);

    return ret == ret2 && subs == subs2;
}

void PropertyLinkBase::unregisterElementReference()
{
    for (auto obj : _ElementRefs) {
        auto it = _ElementRefMap.find(obj);
        if (it != _ElementRefMap.end()) {
            it->second.erase(this);
            if (it->second.empty()) {
                _ElementRefMap.erase(it);
            }
        }
    }
    _ElementRefs.clear();
}

void PropertyLinkBase::unregisterLabelReferences()
{
    for (auto& label : _LabelRefs) {
        auto it = _LabelMap.find(label);
        if (it != _LabelMap.end()) {
            it->second.erase(this);
            if (it->second.empty()) {
                _LabelMap.erase(it);
            }
        }
    }
    _LabelRefs.clear();
}

void PropertyLinkBase::getLabelReferences(std::vector<std::string>& subs, const char* subname)
{
    const char* dot;
    for (; (subname = strchr(subname, '$')) != nullptr; subname = dot + 1) {
        ++subname;
        dot = strchr(subname, '.');
        if (!dot) {
            break;
        }
        subs.emplace_back(subname, dot - subname);
    }
}

void PropertyLinkBase::registerLabelReferences(std::vector<std::string>&& labels, bool reset)
{
    if (reset) {
        unregisterLabelReferences();
    }
    for (auto& label : labels) {
        auto res = _LabelRefs.insert(std::move(label));
        if (res.second) {
            _LabelMap[*res.first].insert(this);
        }
    }
}

void PropertyLinkBase::checkLabelReferences(const std::vector<std::string>& subs, bool reset)
{
    if (reset) {
        unregisterLabelReferences();
    }
    std::vector<std::string> labels;
    for (auto& sub : subs) {
        labels.clear();
        getLabelReferences(labels, sub.c_str());
        registerLabelReferences(std::move(labels), false);
    }
}

std::string PropertyLinkBase::updateLabelReference(const App::DocumentObject* parent,
                                                   const char* subname,
                                                   App::DocumentObject* obj,
                                                   const std::string& ref,
                                                   const char* newLabel)
{
    if (!obj || !obj->isAttachedToDocument() || !parent || !parent->isAttachedToDocument()) {
        return {};
    }

    // Because the label is allowed to be the same across different
    // hierarchies, we have to search for all occurrences, and make sure the
    // referenced sub-object at the found hierarchy is actually the given
    // object.
    for (const char* pos = subname; ((pos = strstr(pos, ref.c_str())) != nullptr);
         pos += ref.size()) {
        auto sub = std::string(subname, pos + ref.size() - subname);
        auto sobj = parent->getSubObject(sub.c_str());
        if (sobj == obj) {
            sub = subname;
            sub.replace(pos + 1 - subname, ref.size() - 2, newLabel);
            return sub;
        }
    }
    return {};
}

std::vector<std::pair<Property*, std::unique_ptr<Property>>>
PropertyLinkBase::updateLabelReferences(App::DocumentObject* obj, const char* newLabel)
{
    std::vector<std::pair<Property*, std::unique_ptr<Property>>> ret;
    if (!obj || !obj->isAttachedToDocument()) {
        return ret;
    }
    auto it = _LabelMap.find(obj->Label.getStrValue());
    if (it == _LabelMap.end()) {
        return ret;
    }
    std::string ref("$");
    ref += obj->Label.getValue();
    ref += '.';
    std::vector<PropertyLinkBase*> props;
    props.reserve(it->second.size());
    props.insert(props.end(), it->second.begin(), it->second.end());
    for (auto prop : props) {
        if (!prop->getContainer()) {
            continue;
        }
        std::unique_ptr<Property> copy(prop->CopyOnLabelChange(obj, ref, newLabel));
        if (copy) {
            ret.emplace_back(prop, std::move(copy));
        }
    }
    return ret;
}

static std::string propertyName(const Property* prop)
{
    if (!prop) {
        return {};
    }
    if (!prop->getContainer() || !prop->hasName()) {
        auto xlink = Base::freecad_dynamic_cast<const PropertyXLink>(prop);
        if (xlink) {
            return propertyName(xlink->parent());
        }
    }
    return prop->getFullName();
}

const std::unordered_set<PropertyLinkBase*>&
PropertyLinkBase::getElementReferences(DocumentObject* feature)
{
    static std::unordered_set<PropertyLinkBase*> none;

    auto it = _ElementRefMap.find(feature);
    if (it == _ElementRefMap.end()) {
        return none;
    }

    return it->second;
}

void PropertyLinkBase::updateElementReferences(DocumentObject* feature, bool reverse)
{
    if (!feature || !feature->getNameInDocument()) {
        return;
    }
    auto it = _ElementRefMap.find(feature);
    if (it == _ElementRefMap.end()) {
        return;
    }
    std::vector<PropertyLinkBase*> props;
    props.reserve(it->second.size());
    props.insert(props.end(), it->second.begin(), it->second.end());
    for (auto prop : props) {
        if (prop->getContainer()) {
            try {
                prop->updateElementReference(feature, reverse, true);
            }
            catch (Base::Exception& e) {
                e.ReportException();
                FC_ERR("Failed to update element reference of " << propertyName(prop));
            }
            catch (std::exception& e) {
                FC_ERR("Failed to update element reference of " << propertyName(prop) << ": "
                                                                << e.what());
            }
        }
    }
}

void PropertyLinkBase::_registerElementReference(App::DocumentObject* obj,
                                                 std::string& sub,
                                                 ShadowSub& shadow)
{
    if (!obj || !obj->getNameInDocument() || sub.empty()) {
        return;
    }
    if (shadow.newName.empty()) {
        _updateElementReference(nullptr, obj, sub, shadow, false);
        return;
    }
    GeoFeature* geo = nullptr;
    const char* element = nullptr;
    ShadowSub elementName;
    GeoFeature::resolveElement(obj,
                               sub.c_str(),
                               elementName,
                               true,
                               GeoFeature::ElementNameType::Export,
                               nullptr,
                               &element,
                               &geo);
    if (!geo || !element || !element[0]) {
        return;
    }

    if (_ElementRefs.insert(geo).second) {
        _ElementRefMap[geo].insert(this);
    }
}

class StringGuard
{
public:
    explicit StringGuard(char* c)
        : c(c)
    {
        v1 = c[0];
        v2 = c[1];
        c[0] = '.';
        c[1] = 0;
    }
    ~StringGuard()
    {
        c[0] = v1;
        c[1] = v2;
    }

    char* c;
    char v1;
    char v2;
};

void PropertyLinkBase::restoreLabelReference(const DocumentObject* obj,
                                             std::string& subname,
                                             ShadowSub* shadow)
{
    std::ostringstream ss;
    char* sub = &subname[0];
    char* next = sub;
    for (char* dot = strchr(next, '.'); dot; next = dot + 1, dot = strchr(next, '.')) {
        if (dot != next && dot[-1] != '@') {
            continue;
        }
        DocumentObject* sobj;
        try {
            StringGuard guard(dot - 1);
            sobj = obj->getSubObject(subname.c_str());
            if (!sobj) {
                FC_ERR("Failed to restore label reference " << obj->getFullName() << '.'
                                                            << ss.str());
                return;
            }
        }
        catch (...) {
            throw;
        }
        ss.write(sub, next - sub);
        ss << '$' << sobj->Label.getStrValue() << '.';
        sub = dot + 1;
    }
    if (sub == subname.c_str()) {
        return;
    }

    size_t count = sub - subname.c_str();
    const auto& newSub = ss.str();
    if (shadow && shadow->oldName.size() >= count) {
        shadow->oldName = newSub + (shadow->oldName.c_str() + count);
    }
    if (shadow && shadow->newName.size() >= count) {
        shadow->newName = newSub + (shadow->newName.c_str() + count);
    }
    subname = newSub + sub;
}

bool PropertyLinkBase::_updateElementReference(DocumentObject* feature,
                                               App::DocumentObject* obj,
                                               std::string& sub,
                                               ShadowSub& shadow,
                                               bool reverse,
                                               bool notify)
{
    if (!obj || !obj->getNameInDocument()) {
        return false;
    }
    ShadowSub elementName;
    const char* subname;
    if (shadow.newName.size()) {
        subname = shadow.newName.c_str();
    }
    else if (shadow.oldName.size()) {
        subname = shadow.oldName.c_str();
    }
    else {
        subname = sub.c_str();
    }
    GeoFeature* geo = nullptr;
    const char* element = nullptr;
    auto ret = GeoFeature::resolveElement(obj,
                                          subname,
                                          elementName,
                                          true,
                                          GeoFeature::ElementNameType::Export,
                                          feature,
                                          &element,
                                          &geo);
    if (!ret || !geo || !element || !element[0]) {
        if (elementName.oldName.size()) {
            shadow.oldName.swap(elementName.oldName);
        }
        return false;
    }

    if (_ElementRefs.insert(geo).second) {
        _ElementRefMap[geo].insert(this);
    }

    if (!reverse) {
        if (elementName.newName.empty()) {
            shadow.oldName.swap(elementName.oldName);
            return false;
        }
        if (shadow == elementName) {
            return false;
        }
    }

    bool missing = GeoFeature::hasMissingElement(elementName.oldName.c_str());
    if (feature == geo && (missing || reverse)) {
        // If the referenced element is missing, or we are generating element
        // map for the first time, or we are re-generating the element map due
        // to version change, i.e. 'reverse', try search by geometry first
        const char* oldElement = Data::findElementName(shadow.oldName.c_str());
        if (!Data::hasMissingElement(oldElement)) {
            const auto& names = geo->searchElementCache(oldElement);
            if (names.size()) {
                missing = false;
                std::string newsub(subname, strlen(subname) - strlen(element));
                newsub += names.front();
                GeoFeature::resolveElement(obj,
                                           newsub.c_str(),
                                           elementName,
                                           true,
                                           GeoFeature::ElementNameType::Export,
                                           feature);
                const auto& oldName = shadow.newName.size() ? shadow.newName : shadow.oldName;
                const auto& newName =
                    elementName.newName.size() ? elementName.newName : elementName.oldName;
                if (oldName != newName) {
                    FC_WARN(propertyName(this)
                            << " auto change element reference " << ret->getFullName() << " "
                            << oldName << " -> " << newName);
                }
            }
        }
    }

    if (notify) {
        aboutToSetValue();
    }

    auto updateSub = [&](const std::string& newSub) {
        if (sub != newSub) {
            // signalUpdateElementReference(sub, newSub);
            sub = newSub;
        }
    };

    if (missing) {
        FC_WARN(propertyName(this)
                << " missing element reference " << ret->getFullName() << " "
                << (elementName.newName.size() ? elementName.newName : elementName.oldName));
        shadow.oldName.swap(elementName.oldName);
    }
    else {
        FC_TRACE(propertyName(this) << " element reference shadow update " << ret->getFullName()
                                    << " " << shadow.newName << " -> " << elementName.newName);
        shadow.swap(elementName);
        if (shadow.newName.size() && Data::hasMappedElementName(sub.c_str())) {
            updateSub(shadow.newName);
        }
    }

    if (reverse) {
        if (shadow.newName.size() && Data::hasMappedElementName(sub.c_str())) {
            updateSub(shadow.newName);
        }
        else {
            updateSub(shadow.oldName);
        }
        return true;
    }
    if (missing) {
        if (sub != shadow.newName) {
            updateSub(shadow.oldName);
        }
        return true;
    }
    auto pos2 = shadow.newName.rfind('.');
    if (pos2 == std::string::npos) {
        return true;
    }
    ++pos2;
    auto pos = sub.rfind('.');
    if (pos == std::string::npos) {
        pos = 0;
    }
    else {
        ++pos;
    }
    if (pos == pos2) {
        if (sub.compare(pos, sub.size() - pos, &shadow.newName[pos2]) != 0) {
            FC_LOG("element reference update " << sub << " -> " << shadow.newName);
            std::string newSub(sub);
            newSub.replace(pos, sub.size() - pos, &shadow.newName[pos2]);
            updateSub(newSub);
        }
    }
    else if (sub != shadow.oldName) {
        FC_LOG("element reference update " << sub << " -> " << shadow.oldName);
        updateSub(shadow.oldName);
    }
    return true;
}

std::pair<DocumentObject*, std::string>
PropertyLinkBase::tryReplaceLink(const PropertyContainer* owner,
                                 DocumentObject* obj,
                                 const DocumentObject* parent,
                                 DocumentObject* oldObj,
                                 DocumentObject* newObj,
                                 const char* subname)
{
    std::pair<DocumentObject*, std::string> res;
    res.first = 0;
    if (!obj) {
        return res;
    }

    if (oldObj == obj) {
        if (owner == parent) {
            res.first = newObj;
            if (subname) {
                res.second = subname;
            }
            return res;
        }
        return res;
    }
    else if (newObj == obj) {
        // This means the new object is already sub-object of this parent
        // (consider a case of swapping the tool and base object of the Cut
        // feature). We'll swap the old and new object.
        return tryReplaceLink(owner, obj, parent, newObj, oldObj, subname);
    }
    if (!subname || !subname[0]) {
        return res;
    }

    App::DocumentObject* prev = obj;
    std::size_t prevPos = 0;
    std::string sub = subname;
    for (auto pos = sub.find('.'); pos != std::string::npos; pos = sub.find('.', pos)) {
        ++pos;
        char c = sub[pos];
        if (c == '.') {
            continue;
        }
        sub[pos] = 0;
        auto sobj = obj->getSubObject(sub.c_str());
        sub[pos] = c;
        if (!sobj) {
            break;
        }
        if (sobj == oldObj) {
            if (prev == parent) {
                if (sub[prevPos] == '$') {
                    sub.replace(prevPos + 1, pos - 1 - prevPos, newObj->Label.getValue());
                }
                else {
                    sub.replace(prevPos, pos - 1 - prevPos, newObj->getNameInDocument());
                }
                res.first = obj;
                res.second = std::move(sub);
                return res;
            }
            break;
        }
        else if (sobj == newObj) {
            return tryReplaceLink(owner, obj, parent, newObj, oldObj, subname);
        }
        else if (prev == parent) {
            break;
        }
        prev = sobj;
        prevPos = pos;
    }
    return res;
}

std::pair<DocumentObject*, std::vector<std::string>>
PropertyLinkBase::tryReplaceLinkSubs(const PropertyContainer* owner,
                                     DocumentObject* obj,
                                     const DocumentObject* parent,
                                     DocumentObject* oldObj,
                                     DocumentObject* newObj,
                                     const std::vector<std::string>& subs)
{
    std::pair<DocumentObject*, std::vector<std::string>> res;
    res.first = 0;
    if (!obj) {
        return res;
    }

    auto r = tryReplaceLink(owner, obj, parent, oldObj, newObj);
    if (r.first) {
        res.first = r.first;
        res.second = subs;
        return res;
    }
    for (auto it = subs.begin(); it != subs.end(); ++it) {
        auto r = tryReplaceLink(owner, obj, parent, oldObj, newObj, it->c_str());
        if (r.first) {
            if (!res.first) {
                res.first = r.first;
                res.second.insert(res.second.end(), subs.begin(), it);
            }
            res.second.push_back(std::move(r.second));
        }
        else if (res.first) {
            res.second.push_back(*it);
        }
    }
    return res;
}

//**************************************************************************
//**************************************************************************
// PropertyLinkListBase
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE_ABSTRACT(App::PropertyLinkListBase, App::PropertyLinkBase)

//**************************************************************************
//**************************************************************************
// PropertyLink
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE(App::PropertyLink, App::PropertyLinkBase)
TYPESYSTEM_SOURCE(App::PropertyLinkChild, App::PropertyLink)
TYPESYSTEM_SOURCE(App::PropertyLinkGlobal, App::PropertyLink)
TYPESYSTEM_SOURCE(App::PropertyLinkHidden, App::PropertyLink)

//**************************************************************************
// Construction/Destruction


PropertyLink::PropertyLink() = default;

PropertyLink::~PropertyLink()
{
    resetLink();
}

//**************************************************************************
// Base class implementer

void PropertyLink::resetLink()
{
    // in case this property gets dynamically removed
    // maintain the back link in the DocumentObject class if it is from a document object
    if (_pcScope != LinkScope::Hidden && _pcLink && getContainer()
        && getContainer()->isDerivedFrom<App::DocumentObject>()) {
        App::DocumentObject* parent = static_cast<DocumentObject*>(getContainer());
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy)) {
            if (_pcLink) {
                _pcLink->_removeBackLink(parent);
            }
        }
    }
    _pcLink = nullptr;
}

void PropertyLink::setValue(App::DocumentObject* lValue)
{
    auto parent = dynamic_cast<App::DocumentObject*>(getContainer());
    if (!testFlag(LinkAllowExternal) && parent && lValue
        && parent->getDocument() != lValue->getDocument()) {
        throw Base::ValueError("PropertyLink does not support external object");
    }

    aboutToSetValue();

    // maintain the back link in the DocumentObject class if it is from a document object
    if (_pcScope != LinkScope::Hidden && parent) {
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy)) {
            if (_pcLink) {
                _pcLink->_removeBackLink(parent);
            }
            if (lValue) {
                lValue->_addBackLink(parent);
            }
        }
    }

    _pcLink = lValue;
    hasSetValue();
}

App::DocumentObject* PropertyLink::getValue() const
{
    return _pcLink;
}

App::DocumentObject* PropertyLink::getValue(Base::Type t) const
{
    return (_pcLink && _pcLink->isDerivedFrom(t)) ? _pcLink : nullptr;
}

PyObject* PropertyLink::getPyObject()
{
    if (_pcLink) {
        return _pcLink->getPyObject();
    }
    else {
        Py_Return;
    }
}

void PropertyLink::setPyObject(PyObject* value)
{
    Base::PyTypeCheck(&value, &DocumentObjectPy::Type);
    if (value) {
        DocumentObjectPy* pcObject = static_cast<DocumentObjectPy*>(value);
        setValue(pcObject->getDocumentObjectPtr());
    }
    else {
        setValue(nullptr);
    }
}

void PropertyLink::Save(Base::Writer& writer) const
{
    writer.Stream() << writer.ind() << "<Link value=\"" << (_pcLink ? _pcLink->getExportName() : "")
                    << "\"/>" << std::endl;
}

void PropertyLink::Restore(Base::XMLReader& reader)
{
    // read my element
    reader.readElement("Link");
    // get the value of my attribute
    std::string name = reader.getName(reader.getAttribute("value"));

    // Property not in a DocumentObject!
    assert(getContainer()->isDerivedFrom<App::DocumentObject>());

    if (!name.empty()) {
        DocumentObject* parent = static_cast<DocumentObject*>(getContainer());

        App::Document* document = parent->getDocument();
        DocumentObject* object = document ? document->getObject(name.c_str()) : nullptr;
        if (!object) {
            if (reader.isVerbose()) {
                Base::Console().Warning("Lost link to '%s' while loading, maybe "
                                        "an object was not loaded correctly\n",
                                        name.c_str());
            }
        }
        else if (parent == object) {
            if (reader.isVerbose()) {
                Base::Console().Warning("Object '%s' links to itself, nullify it\n", name.c_str());
            }
            object = nullptr;
        }

        setValue(object);
    }
    else {
        setValue(nullptr);
    }
}

Property* PropertyLink::Copy() const
{
    PropertyLink* p = new PropertyLink();
    p->_pcLink = _pcLink;
    return p;
}

void PropertyLink::Paste(const Property& from)
{
    if (!from.isDerivedFrom<PropertyLink>()) {
        throw Base::TypeError("Incompatible property to paste to");
    }

    setValue(static_cast<const PropertyLink&>(from)._pcLink);
}

void PropertyLink::getLinks(std::vector<App::DocumentObject*>& objs,
                            bool all,
                            std::vector<std::string>* subs,
                            bool newStyle) const
{
    (void)newStyle;
    (void)subs;
    if ((all || _pcScope != LinkScope::Hidden) && _pcLink && _pcLink->isAttachedToDocument()) {
        objs.push_back(_pcLink);
    }
}

void PropertyLink::getLinksTo(std::vector<App::ObjectIdentifier>& identifiers,
                              App::DocumentObject* obj,
                              const char* subname,
                              bool all) const
{
    (void)subname;
    if (!all && _pcScope == LinkScope::Hidden) {
        return;  // Don't get hidden links unless all is specified.
    }
    if (obj && _pcLink == obj) {
        identifiers.emplace_back(*this);
    }
}

void PropertyLink::breakLink(App::DocumentObject* obj, bool clear)
{
    if (_pcLink == obj || (clear && getContainer() == obj)) {
        setValue(nullptr);
    }
}

bool PropertyLink::adjustLink(const std::set<App::DocumentObject*>& inList)
{
    (void)inList;
    return false;
}

Property* PropertyLink::CopyOnLinkReplace(const App::DocumentObject* parent,
                                          App::DocumentObject* oldObj,
                                          App::DocumentObject* newObj) const
{
    auto res = tryReplaceLink(getContainer(), _pcLink, parent, oldObj, newObj);
    if (res.first) {
        auto p = new PropertyLink();
        p->_pcLink = res.first;
        return p;
    }
    return nullptr;
}

//**************************************************************************
// PropertyLinkList
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE(App::PropertyLinkList, App::PropertyLinkListBase)
TYPESYSTEM_SOURCE(App::PropertyLinkListChild, App::PropertyLinkList)
TYPESYSTEM_SOURCE(App::PropertyLinkListGlobal, App::PropertyLinkList)
TYPESYSTEM_SOURCE(App::PropertyLinkListHidden, App::PropertyLinkList)

//**************************************************************************
// Construction/Destruction


PropertyLinkList::PropertyLinkList() = default;

PropertyLinkList::~PropertyLinkList()
{
    // in case this property gety dynamically removed

    // maintain the back link in the DocumentObject class
    if (_pcScope != LinkScope::Hidden && !_lValueList.empty() && getContainer()
        && getContainer()->isDerivedFrom<App::DocumentObject>()) {
        App::DocumentObject* parent = static_cast<DocumentObject*>(getContainer());
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy)) {
            for (auto* obj : _lValueList) {
                if (obj) {
                    obj->_removeBackLink(parent);
                }
            }
        }
    }
}

void PropertyLinkList::setSize(int newSize)
{
    for (int i = newSize; i < (int)_lValueList.size(); ++i) {
        auto obj = _lValueList[i];
        if (!obj || !obj->isAttachedToDocument()) {
            continue;
        }
        _nameMap.erase(obj->getNameInDocument());

        if (_pcScope != LinkScope::Hidden) {
            obj->_removeBackLink(static_cast<DocumentObject*>(getContainer()));
        }
    }
    _lValueList.resize(newSize);
}

void PropertyLinkList::setSize(int newSize, const_reference def)
{
    auto oldSize = getSize();
    setSize(newSize);
    for (auto i = oldSize; i < newSize; ++i) {
        _lValueList[i] = def;
    }
}

void PropertyLinkList::set1Value(int idx, DocumentObject* const& value)
{
    DocumentObject* obj = nullptr;
    if (idx >= 0 && idx < (int)_lValueList.size()) {
        obj = _lValueList[idx];
        if (obj == value) {
            return;
        }
    }

    if (!value || !value->isAttachedToDocument()) {
        throw Base::ValueError("invalid document object");
    }

    _nameMap.clear();

    if (getContainer() && getContainer()->isDerivedFrom<App::DocumentObject>()) {
        App::DocumentObject* parent = static_cast<DocumentObject*>(getContainer());
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
            if (obj) {
                obj->_removeBackLink(static_cast<DocumentObject*>(getContainer()));
            }
            if (value) {
                value->_addBackLink(static_cast<DocumentObject*>(getContainer()));
            }
        }
    }

    inherited::set1Value(idx, value);
}

void PropertyLinkList::setValues(const std::vector<DocumentObject*>& value)
{
    if (value.size() == 1 && !value[0]) {
        // one null element means clear, as backward compatibility for old code
        setValues(std::vector<DocumentObject*>());
        return;
    }

    auto parent = Base::freecad_dynamic_cast<App::DocumentObject>(getContainer());
    for (auto obj : value) {
        if (!obj || !obj->isAttachedToDocument()) {
            throw Base::ValueError("PropertyLinkList: invalid document object");
        }
        if (!testFlag(LinkAllowExternal) && parent && parent->getDocument() != obj->getDocument()) {
            throw Base::ValueError("PropertyLinkList does not support external object");
        }
    }
    _nameMap.clear();

    // maintain the back link in the DocumentObject class
    if (parent) {
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
            for (auto* obj : _lValueList) {
                if (obj) {
                    obj->_removeBackLink(parent);
                }
            }
            for (auto* obj : value) {
                if (obj) {
                    obj->_addBackLink(parent);
                }
            }
        }
    }

    inherited::setValues(value);
}

PyObject* PropertyLinkList::getPyObject()
{
    int count = getSize();
#if 0  // FIXME: Should switch to tuple
    Py::Tuple sequence(count);
#else
    Py::List sequence(count);
#endif
    for (int i = 0; i < count; i++) {
        auto obj = _lValueList[i];
        if (obj && obj->isAttachedToDocument()) {
            sequence.setItem(i, Py::asObject(_lValueList[i]->getPyObject()));
        }
        else {
            sequence.setItem(i, Py::None());
        }
    }

    return Py::new_reference_to(sequence);
}

DocumentObject* PropertyLinkList::getPyValue(PyObject* item) const
{
    Base::PyTypeCheck(&item, &DocumentObjectPy::Type);

    return item ? static_cast<DocumentObjectPy*>(item)->getDocumentObjectPtr() : nullptr;
}

void PropertyLinkList::Save(Base::Writer& writer) const
{
    writer.Stream() << writer.ind() << "<LinkList count=\"" << getSize() << "\">" << endl;
    writer.incInd();
    for (int i = 0; i < getSize(); i++) {
        DocumentObject* obj = _lValueList[i];
        if (obj) {
            writer.Stream() << writer.ind() << "<Link value=\"" << obj->getExportName() << "\"/>"
                            << endl;
        }
        else {
            writer.Stream() << writer.ind() << "<Link value=\"\"/>" << endl;
        }
    }

    writer.decInd();
    writer.Stream() << writer.ind() << "</LinkList>" << endl;
}

void PropertyLinkList::Restore(Base::XMLReader& reader)
{
    // read my element
    reader.readElement("LinkList");
    // get the value of my attribute
    int count = reader.getAttributeAsInteger("count");
    App::PropertyContainer* container = getContainer();
    if (!container) {
        throw Base::RuntimeError("Property is not part of a container");
    }
    if (!container->isDerivedFrom<App::DocumentObject>()) {
        std::stringstream str;
        str << "Container is not a document object (" << container->getTypeId().getName() << ")";
        throw Base::TypeError(str.str());
    }

    std::vector<DocumentObject*> values;
    values.reserve(count);
    for (int i = 0; i < count; i++) {
        reader.readElement("Link");
        std::string name = reader.getName(reader.getAttribute("value"));
        // In order to do copy/paste it must be allowed to have defined some
        // referenced objects in XML which do not exist anymore in the new
        // document. Thus, we should silently ignore this.
        // Property not in an object!
        DocumentObject* father = static_cast<DocumentObject*>(getContainer());
        App::Document* document = father->getDocument();
        DocumentObject* child = document ? document->getObject(name.c_str()) : nullptr;
        if (child) {
            values.push_back(child);
        }
        else if (reader.isVerbose()) {
            FC_WARN("Lost link to " << (document ? document->getName() : "") << " " << name
                                    << " while loading, maybe an object was not loaded correctly");
        }
    }

    reader.readEndElement("LinkList");

    // assignment
    setValues(values);
}

Property* PropertyLinkList::CopyOnLinkReplace(const App::DocumentObject* parent,
                                              App::DocumentObject* oldObj,
                                              App::DocumentObject* newObj) const
{
    std::vector<DocumentObject*> links;
    bool copied = false;
    bool found = false;
    for (auto it = _lValueList.begin(); it != _lValueList.end(); ++it) {
        auto res = tryReplaceLink(getContainer(), *it, parent, oldObj, newObj);
        if (res.first) {
            found = true;
            if (!copied) {
                copied = true;
                links.insert(links.end(), _lValueList.begin(), it);
            }
            links.push_back(res.first);
        }
        else if (*it == newObj) {
            // in case newObj already exists here, we shall remove all existing
            // entry, and insert it to take over oldObj's position.
            if (!copied) {
                copied = true;
                links.insert(links.end(), _lValueList.begin(), it);
            }
        }
        else if (copied) {
            links.push_back(*it);
        }
    }
    if (!found) {
        return nullptr;
    }
    auto p = new PropertyLinkList();
    p->_lValueList = std::move(links);
    return p;
}

Property* PropertyLinkList::Copy() const
{
    PropertyLinkList* p = new PropertyLinkList();
    p->_lValueList = _lValueList;
    return p;
}

void PropertyLinkList::Paste(const Property& from)
{
    if (!from.isDerivedFrom<PropertyLinkList>()) {
        throw Base::TypeError("Incompatible property to paste to");
    }

    setValues(static_cast<const PropertyLinkList&>(from)._lValueList);
}

unsigned int PropertyLinkList::getMemSize() const
{
    return static_cast<unsigned int>(_lValueList.size() * sizeof(App::DocumentObject*));
}


DocumentObject* PropertyLinkList::find(const char* name, int* pindex) const
{
    const int DONT_MAP_UNDER = 10;
    if (!name) {
        return nullptr;
    }
    if (_lValueList.size() <= DONT_MAP_UNDER) {
        int index = -1;
        for (auto obj : _lValueList) {
            ++index;
            if (obj && obj->getNameInDocument() && boost::equals(name, obj->getNameInDocument())) {
                if (pindex) {
                    *pindex = index;
                }
                return obj;
            }
        }
        return nullptr;
    }
    // We're using a map.  Do we need to (re)create it?
    if (_nameMap.empty() || _nameMap.size() > _lValueList.size()) {
        _nameMap.clear();
        for (int i = 0; i < (int)_lValueList.size(); ++i) {
            auto obj = _lValueList[i];
            if (obj && obj->isAttachedToDocument()) {
                _nameMap[obj->getNameInDocument()] = i;
            }
        }
    }
    // Now lookup up in that map
    auto it = _nameMap.find(name);
    if (it == _nameMap.end()) {
        return nullptr;
    }
    if (pindex) {
        *pindex = it->second;
    }
    return _lValueList[it->second];
}

DocumentObject* PropertyLinkList::findUsingMap(const std::string& name, int* pindex) const
{
    if (_nameMap.size() == _lValueList.size()) {
        auto it = _nameMap.find(name);
        if (it == _nameMap.end()) {
            return nullptr;
        }
        if (pindex) {
            *pindex = it->second;
        }
        return _lValueList[it->second];
    }
    return find(name.c_str(), pindex);
}

void PropertyLinkList::getLinks(std::vector<App::DocumentObject*>& objs,
                                bool all,
                                std::vector<std::string>* subs,
                                bool newStyle) const
{
    (void)subs;
    (void)newStyle;
    if (all || _pcScope != LinkScope::Hidden) {
        objs.reserve(objs.size() + _lValueList.size());
        for (auto obj : _lValueList) {
            if (obj && obj->isAttachedToDocument()) {
                objs.push_back(obj);
            }
        }
    }
}

void PropertyLinkList::getLinksTo(std::vector<App::ObjectIdentifier>& identifiers,
                                  App::DocumentObject* obj,
                                  const char* subname,
                                  bool all) const
{
    (void)subname;
    if (!obj || (!all && _pcScope == LinkScope::Hidden)) {
        return;
    }
    int i = -1;
    for (auto docObj : _lValueList) {
        ++i;
        if (docObj == obj) {
            identifiers.emplace_back(*this, i);
            break;
        }
    }
}

void PropertyLinkList::breakLink(App::DocumentObject* obj, bool clear)
{
    if (clear && getContainer() == obj) {
        setValues({});
        return;
    }
    std::vector<App::DocumentObject*> values;
    values.reserve(_lValueList.size());
    for (auto o : _lValueList) {
        if (o != obj) {
            values.push_back(o);
        }
    }
    if (values.size() != _lValueList.size()) {
        setValues(values);
    }
}

bool PropertyLinkList::adjustLink(const std::set<App::DocumentObject*>& inList)
{
    (void)inList;
    return false;
}


//**************************************************************************
// PropertyLinkSub
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE(App::PropertyLinkSub, App::PropertyLinkBase)
TYPESYSTEM_SOURCE(App::PropertyLinkSubChild, App::PropertyLinkSub)
TYPESYSTEM_SOURCE(App::PropertyLinkSubGlobal, App::PropertyLinkSub)
TYPESYSTEM_SOURCE(App::PropertyLinkSubHidden, App::PropertyLinkSub)

//**************************************************************************
// Construction/Destruction


PropertyLinkSub::PropertyLinkSub() = default;

PropertyLinkSub::~PropertyLinkSub()
{
    // in case this property is dynamically removed

    if (_pcLinkSub && getContainer()
        && getContainer()->isDerivedFrom<App::DocumentObject>()) {
        App::DocumentObject* parent = static_cast<DocumentObject*>(getContainer());
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
            if (_pcLinkSub) {
                _pcLinkSub->_removeBackLink(parent);
            }
        }
    }
}

void PropertyLinkSub::setSyncSubObject(bool enable)
{
    _Flags.set((std::size_t)LinkSyncSubObject, enable);
}

void PropertyLinkSub::setValue(App::DocumentObject* lValue,
                               const std::vector<std::string>& SubList,
                               std::vector<ShadowSub>&& shadows)
{
    setValue(lValue, std::vector<std::string>(SubList), std::move(shadows));
}

void PropertyLinkSub::setValue(App::DocumentObject* lValue,
                               std::vector<std::string>&& subs,
                               std::vector<ShadowSub>&& shadows)
{
    auto parent = Base::freecad_dynamic_cast<App::DocumentObject>(getContainer());
    if (lValue) {
        if (!lValue->isAttachedToDocument()) {
            throw Base::ValueError("PropertyLinkSub: invalid document object");
        }
        if (!testFlag(LinkAllowExternal) && parent
            && parent->getDocument() != lValue->getDocument()) {
            throw Base::ValueError("PropertyLinkSub does not support external object");
        }
    }
    aboutToSetValue();

    if (parent) {
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
            if (_pcLinkSub) {
                _pcLinkSub->_removeBackLink(parent);
            }
            if (lValue) {
                lValue->_addBackLink(parent);
            }
        }
    }

    _pcLinkSub = lValue;
    _cSubList = std::move(subs);
    if (shadows.size() == _cSubList.size()) {
        _ShadowSubList = std::move(shadows);
        onContainerRestored();  // re-register element references
    }
    else {
        updateElementReference(nullptr);
    }
    checkLabelReferences(_cSubList);
    hasSetValue();
}

App::DocumentObject* PropertyLinkSub::getValue() const
{
    return _pcLinkSub;
}

const std::vector<std::string>& PropertyLinkSub::getSubValues() const
{
    return _cSubList;
}

static inline const std::string& getSubNameWithStyle(const std::string& subName,
                                                     const PropertyLinkBase::ShadowSub& shadow,
                                                     bool newStyle,
                                                     std::string& tmp)
{
    if (!newStyle) {
        if (!shadow.oldName.empty()) {
            return shadow.oldName;
        }
    }
    else if (!shadow.newName.empty()) {
        if (Data::hasMissingElement(shadow.oldName.c_str())) {
            auto pos = shadow.newName.rfind('.');
            if (pos != std::string::npos) {
                tmp = shadow.newName.substr(0, pos + 1);
                tmp += shadow.oldName;
                return tmp;
            }
        }
        return shadow.newName;
    }
    return subName;
}

std::vector<std::string> PropertyLinkSub::getSubValues(bool newStyle) const
{
    assert(_cSubList.size() == _ShadowSubList.size());
    std::vector<std::string> ret;
    ret.reserve(_cSubList.size());
    std::string tmp;
    for (size_t i = 0; i < _ShadowSubList.size(); ++i) {
        ret.push_back(getSubNameWithStyle(_cSubList[i], _ShadowSubList[i], newStyle, tmp));
    }
    return ret;
}

std::vector<std::string> PropertyLinkSub::getSubValuesStartsWith(const char* starter,
                                                                 bool newStyle) const
{
    assert(_cSubList.size() == _ShadowSubList.size());
    std::vector<std::string> ret;
    std::string tmp;
    for (size_t i = 0; i < _ShadowSubList.size(); ++i) {
        const auto& sub = getSubNameWithStyle(_cSubList[i], _ShadowSubList[i], newStyle, tmp);
        auto element = Data::findElementName(sub.c_str());
        if (element && boost::starts_with(element, starter)) {
            ret.emplace_back(element);
        }
    }
    return ret;
}

App::DocumentObject* PropertyLinkSub::getValue(Base::Type t) const
{
    return (_pcLinkSub && _pcLinkSub->isDerivedFrom(t)) ? _pcLinkSub : nullptr;
}

PyObject* PropertyLinkSub::getPyObject()
{
    Py::Tuple tup(2);
    Py::List list(static_cast<int>(_cSubList.size()));
    if (_pcLinkSub) {
        tup[0] = Py::asObject(_pcLinkSub->getPyObject());
        int i = 0;
        for (auto& sub : getSubValues(testFlag(LinkNewElement))) {
            list[i++] = Py::String(sub);
        }
        tup[1] = list;
        return Py::new_reference_to(tup);
    }
    else {
        return Py::new_reference_to(Py::None());
    }
}

void PropertyLinkSub::setPyObject(PyObject* value)
{
    if (PyObject_TypeCheck(value, &(DocumentObjectPy::Type))) {
        DocumentObjectPy* pcObject = static_cast<DocumentObjectPy*>(value);
        setValue(pcObject->getDocumentObjectPtr());
    }
    else if (PyTuple_Check(value) || PyList_Check(value)) {
        Py::Sequence seq(value);
        if (seq.size() == 0) {
            setValue(nullptr);
        }
        else if (seq.size() != 2) {
            throw Base::ValueError("Expect input sequence of size 2");
        }
        else if (PyObject_TypeCheck(seq[0].ptr(), &(DocumentObjectPy::Type))) {
            DocumentObjectPy* pcObj = static_cast<DocumentObjectPy*>(seq[0].ptr());
            static const char* errMsg =
                "type of second element in tuple must be str or sequence of str";
            PropertyString propString;
            if (seq[1].isString()) {
                std::vector<std::string> vals;
                propString.setPyObject(seq[1].ptr());
                vals.emplace_back(propString.getValue());
                setValue(pcObj->getDocumentObjectPtr(), std::move(vals));
            }
            else if (seq[1].isSequence()) {
                Py::Sequence list(seq[1]);
                std::vector<std::string> vals(list.size());
                unsigned int i = 0;
                for (Py::Sequence::iterator it = list.begin(); it != list.end(); ++it, ++i) {
                    if (!(*it).isString()) {
                        throw Base::TypeError(errMsg);
                    }
                    propString.setPyObject((*it).ptr());
                    vals[i] = propString.getValue();
                }
                setValue(pcObj->getDocumentObjectPtr(), std::move(vals));
            }
            else {
                throw Base::TypeError(errMsg);
            }
        }
        else {
            std::string error =
                std::string("type of first element in tuple must be 'DocumentObject', not ");
            error += seq[0].ptr()->ob_type->tp_name;
            throw Base::TypeError(error);
        }
    }
    else if (Py_None == value) {
        setValue(nullptr);
    }
    else {
        std::string error = std::string(
            "type must be 'DocumentObject', 'NoneType' or ('DocumentObject',['String',]) not ");
        error += value->ob_type->tp_name;
        throw Base::TypeError(error);
    }
}

static bool updateLinkReference(App::PropertyLinkBase* prop,
                                App::DocumentObject* feature,
                                bool reverse,
                                bool notify,
                                App::DocumentObject* link,
                                std::vector<std::string>& subs,
                                std::vector<int>& mapped,
                                std::vector<PropertyLinkBase::ShadowSub>& shadows)
{
    if (!feature) {
        shadows.clear();
        prop->unregisterElementReference();
    }
    shadows.resize(subs.size());
    if (!link || !link->isAttachedToDocument()) {
        return false;
    }
    auto owner = dynamic_cast<DocumentObject*>(prop->getContainer());
    if (owner && owner->isRestoring()) {
        return false;
    }
    int i = 0;
    bool touched = false;
    for (auto& sub : subs) {
        if (prop->_updateElementReference(feature,
                                          link,
                                          sub,
                                          shadows[i++],
                                          reverse,
                                          notify && !touched)) {
            touched = true;
        }
    }
    if (!touched) {
        return false;
    }
    for (int idx : mapped) {
        if (idx < (int)subs.size() && !shadows[idx].newName.empty()) {
            subs[idx] = shadows[idx].newName;
        }
    }
    mapped.clear();
    if (owner && feature) {
        owner->onUpdateElementReference(prop);
    }
    return true;
}

void PropertyLinkSub::afterRestore()
{
    _ShadowSubList.resize(_cSubList.size());
    if (!testFlag(LinkRestoreLabel) || !_pcLinkSub || !_pcLinkSub->isAttachedToDocument()) {
        return;
    }
    setFlag(LinkRestoreLabel, false);
    for (std::size_t i = 0; i < _cSubList.size(); ++i) {
        restoreLabelReference(_pcLinkSub, _cSubList[i], &_ShadowSubList[i]);
    }
}

void PropertyLinkSub::onContainerRestored()
{
    unregisterElementReference();
    if (!_pcLinkSub || !_pcLinkSub->isAttachedToDocument()) {
        return;
    }
    for (std::size_t i = 0; i < _cSubList.size(); ++i) {
        _registerElementReference(_pcLinkSub, _cSubList[i], _ShadowSubList[i]);
    }
}

void PropertyLinkSub::updateElementReference(DocumentObject* feature, bool reverse, bool notify)
{
    if (!updateLinkReference(this,
                             feature,
                             reverse,
                             notify,
                             _pcLinkSub,
                             _cSubList,
                             _mapped,
                             _ShadowSubList)) {
        return;
    }
    if (notify) {
        hasSetValue();
    }
}

bool PropertyLinkSub::referenceChanged() const
{
    return !_mapped.empty();
}

std::string
PropertyLinkBase::importSubName(Base::XMLReader& reader, const char* sub, bool& restoreLabel)
{
    if (!reader.doNameMapping()) {
        return sub;
    }
    std::ostringstream str;
    for (const char* dot = strchr(sub, '.'); dot; sub = dot + 1, dot = strchr(sub, '.')) {
        size_t count = dot - sub;
        const char* tail = ".";
        if (count && dot[-1] == '@') {
            // tail=='@' means we are exporting a label reference. So retain
            // this marker so that the label can be restored in afterRestore().
            tail = "@.";
            --count;
            restoreLabel = true;
        }
        str << reader.getName(std::string(sub, count).c_str()) << tail;
    }
    str << sub;
    return str.str();
}

const char* PropertyLinkBase::exportSubName(std::string& output,
                                            const App::DocumentObject* obj,
                                            const char* sub,
                                            bool first_obj)
{
    std::ostringstream str;
    const char* res = sub;

    if (!sub || !sub[0]) {
        return res;
    }

    bool touched = false;
    if (first_obj) {
        auto dot = strchr(sub, '.');
        if (!dot) {
            return res;
        }
        const char* hash;
        for (hash = sub; hash < dot && *hash != '#'; ++hash) {}
        App::Document* doc = nullptr;
        if (*hash == '#') {
            doc = GetApplication().getDocument(std::string(sub, hash - sub).c_str());
        }
        else {
            hash = nullptr;
            if (obj && obj->isAttachedToDocument()) {
                doc = obj->getDocument();
            }
        }
        if (!doc) {
            FC_ERR("Failed to get document for the first object in " << sub);
            return res;
        }
        obj = doc->getObject(std::string(sub, dot - sub).c_str());
        if (!obj || !obj->isAttachedToDocument()) {
            return res;
        }
        if (hash) {
            if (!obj->isExporting()) {
                str << doc->getName() << '#';
            }
            sub = hash + 1;
        }
    }
    else if (!obj || !obj->isAttachedToDocument()) {
        return res;
    }

    for (const char* dot = strchr(sub, '.'); dot; sub = dot + 1, dot = strchr(sub, '.')) {
        // name with trailing '.'
        auto name = std::string(sub, dot - sub + 1);
        if (first_obj) {
            first_obj = false;
        }
        else {
            obj = obj->getSubObject(name.c_str());
        }
        if (!obj || !obj->isAttachedToDocument()) {
            FC_WARN("missing sub object '" << name << "' in '" << sub << "'");
            break;
        }
        if (obj->isExporting()) {
            if (name[0] == '$') {
                if (name.compare(1, name.size() - 2, obj->Label.getValue()) != 0) {
                    str << obj->getExportName(true) << "@.";
                    touched = true;
                    continue;
                }
            }
            else if (name.compare(0, name.size() - 1, obj->getNameInDocument()) == 0) {
                str << obj->getExportName(true) << '.';
                touched = true;
                continue;
            }
        }
        str << name;
    }
    if (!touched) {
        return res;
    }
    str << sub;
    output = str.str();
    return output.c_str();
}

App::DocumentObject* PropertyLinkBase::tryImport(const App::Document* doc,
                                                 const App::DocumentObject* obj,
                                                 const std::map<std::string, std::string>& nameMap)
{
    if (doc && obj && obj->isAttachedToDocument()) {
        auto it = nameMap.find(obj->getExportName(true));
        if (it != nameMap.end()) {
            obj = doc->getObject(it->second.c_str());
            if (!obj) {
                FC_THROWM(Base::RuntimeError, "Cannot find import object " << it->second);
            }
        }
    }
    return const_cast<DocumentObject*>(obj);
}

std::string PropertyLinkBase::tryImportSubName(const App::DocumentObject* obj,
                                               const char* _subname,
                                               const App::Document* doc,
                                               const std::map<std::string, std::string>& nameMap)
{
    if (!doc || !obj || !obj->isAttachedToDocument()) {
        return {};
    }

    std::ostringstream ss;
    std::string subname(_subname);
    char* sub = &subname[0];
    char* next = sub;
    for (char* dot = strchr(next, '.'); dot; next = dot + 1, dot = strchr(next, '.')) {
        StringGuard guard(dot);
        auto sobj = obj->getSubObject(subname.c_str());
        if (!sobj) {
            FC_ERR("Failed to restore label reference " << obj->getFullName() << '.' << subname);
            return {};
        }
        dot[0] = 0;
        if (next[0] == '$') {
            if (strcmp(next + 1, sobj->Label.getValue()) != 0) {
                continue;
            }
        }
        else if (strcmp(next, sobj->getNameInDocument()) != 0) {
            continue;
        }
        auto it = nameMap.find(sobj->getExportName(true));
        if (it == nameMap.end()) {
            continue;
        }
        auto imported = doc->getObject(it->second.c_str());
        if (!imported) {
            FC_THROWM(RuntimeError, "Failed to find imported object " << it->second);
        }
        ss.write(sub, next - sub);
        if (next[0] == '$') {
            ss << '$' << imported->Label.getStrValue() << '.';
        }
        else {
            ss << it->second << '.';
        }
        sub = dot + 1;
    }
    if (sub != subname.c_str()) {
        return ss.str();
    }
    return {};
}

void PropertyLinkBase::_getLinksTo(std::vector<App::ObjectIdentifier>& identifiers,
                                   App::DocumentObject* obj,
                                   const char* subname,
                                   const std::vector<std::string>& subs,
                                   const std::vector<PropertyLinkBase::ShadowSub>& shadows) const
{
    if (!subname) {
        identifiers.emplace_back(*this);
        return;
    }
    App::SubObjectT objT(obj, subname);
    auto subObject = objT.getSubObject();
    auto subElement = objT.getOldElementName();

    int i = -1;
    for (const auto& sub : subs) {
        ++i;
        if (sub == subname) {
            identifiers.emplace_back(*this);
            return;
        }
        if (!subObject) {
            continue;
        }
        // After above, there is a subobject and the subname doesn't match our current entry
        App::SubObjectT sobjT(obj, sub.c_str());
        if (sobjT.getSubObject() == subObject && sobjT.getOldElementName() == subElement) {
            identifiers.emplace_back(*this);
            return;
        }
        // And the oldElementName ( short, I.E. "Edge5" ) doesn't match.
        if (i < (int)shadows.size()) {
            const auto& [shadowNewName, shadowOldName] = shadows[i];
            if (shadowNewName == subname || shadowOldName == subname) {
                identifiers.emplace_back(*this);
                return;
            }
            if (!subObject) {
                continue;
            }
            App::SubObjectT shadowobjT(obj,
                                       shadowNewName.empty() ? shadowOldName.c_str()
                                                             : shadowNewName.c_str());
            if (shadowobjT.getSubObject() == subObject
                && shadowobjT.getOldElementName() == subElement) {
                identifiers.emplace_back(*this);
                return;
            }
        }
    }
}

#define ATTR_SHADOWED "shadowed"
#define ATTR_SHADOW "shadow"
#define ATTR_MAPPED "mapped"

#define IGNORE_SHADOW false

void PropertyLinkSub::Save(Base::Writer& writer) const
{
    assert(_cSubList.size() == _ShadowSubList.size());

    std::string internal_name;
    // it can happen that the object is still alive but is not part of the document anymore and thus
    // returns 0
    if (_pcLinkSub && _pcLinkSub->isAttachedToDocument()) {
        internal_name = _pcLinkSub->getExportName();
    }
    writer.Stream() << writer.ind() << "<LinkSub value=\"" << internal_name << "\" count=\""
                    << _cSubList.size();
    writer.Stream() << "\">" << std::endl;
    writer.incInd();
    auto owner = dynamic_cast<DocumentObject*>(getContainer());
    bool exporting = owner && owner->isExporting();
    for (unsigned int i = 0; i < _cSubList.size(); i++) {
        const auto& shadow = _ShadowSubList[i];
        // shadow.oldName stores the old style element name. For backward
        // compatibility reason, we shall store the old name into attribute
        // 'value' whenever possible.
        const auto& sub = shadow.oldName.empty() ? _cSubList[i] : shadow.oldName;
        writer.Stream() << writer.ind() << "<Sub value=\"";
        if (exporting) {
            std::string exportName;
            writer.Stream() << encodeAttribute(exportSubName(exportName, _pcLinkSub, sub.c_str()));
            if (!shadow.oldName.empty() && shadow.newName == _cSubList[i]) {
                writer.Stream() << "\" " ATTR_MAPPED "=\"1";
            }
        }
        else {
            writer.Stream() << encodeAttribute(sub);
            if (!_cSubList[i].empty()) {
                if (sub != _cSubList[i]) {
                    // Stores the actual value that is shadowed. For new version FC,
                    // we will restore this shadowed value instead.
                    writer.Stream() << "\" " ATTR_SHADOWED "=\"" << encodeAttribute(_cSubList[i]);
                }
                else if (!shadow.newName.empty()) {
                    // Here means the user set value is old style element name.
                    // We shall then store the shadow somewhere else.
                    writer.Stream() << "\" " ATTR_SHADOW "=\"" << encodeAttribute(shadow.newName);
                }
            }
        }
        writer.Stream() << "\"/>" << endl;
    }
    writer.decInd();
    writer.Stream() << writer.ind() << "</LinkSub>" << endl;
}

void PropertyLinkSub::Restore(Base::XMLReader& reader)
{
    // read my element
    reader.readElement("LinkSub");
    // get the values of my attributes
    std::string name = reader.getName(reader.getAttribute("value"));
    int count = reader.getAttributeAsInteger("count");

    // Property not in a DocumentObject!
    assert(getContainer()->isDerivedFrom<App::DocumentObject>());
    App::Document* document = static_cast<DocumentObject*>(getContainer())->getDocument();

    DocumentObject* pcObject = nullptr;
    if (!name.empty()) {
        pcObject = document ? document->getObject(name.c_str()) : nullptr;
        if (!pcObject) {
            if (reader.isVerbose()) {
                FC_WARN("Lost link to "
                        << name << " while loading, maybe an object was not loaded correctly");
            }
        }
    }

    std::vector<int> mapped;
    std::vector<std::string> values(count);
    std::vector<ShadowSub> shadows(count);
    bool restoreLabel = false;
    // Sub may store '.' separated object names, so be aware of the possible mapping when import
    for (int i = 0; i < count; i++) {
        reader.readElement("Sub");
        shadows[i].oldName = importSubName(reader, reader.getAttribute("value"), restoreLabel);
        if (reader.hasAttribute(ATTR_SHADOWED) && !IGNORE_SHADOW) {
            values[i] = shadows[i].newName =
                importSubName(reader, reader.getAttribute(ATTR_SHADOWED), restoreLabel);
        }
        else {
            values[i] = shadows[i].oldName;
            if (reader.hasAttribute(ATTR_SHADOW) && !IGNORE_SHADOW) {
                shadows[i].newName =
                    importSubName(reader, reader.getAttribute(ATTR_SHADOW), restoreLabel);
            }
        }
        if (reader.hasAttribute(ATTR_MAPPED)) {
            mapped.push_back(i);
        }
    }
    setFlag(LinkRestoreLabel, restoreLabel);

    reader.readEndElement("LinkSub");

    if (pcObject) {
        setValue(pcObject, std::move(values), std::move(shadows));
        _mapped = std::move(mapped);
    }
    else {
        setValue(nullptr);
    }
}

template<class Func, class... Args>
std::vector<std::string> updateLinkSubs(const App::DocumentObject* obj,
                                        const std::vector<std::string>& subs,
                                        Func* f,
                                        Args&&... args)
{
    if (!obj || !obj->isAttachedToDocument()) {
        return {};
    }

    std::vector<std::string> res;
    for (auto it = subs.begin(); it != subs.end(); ++it) {
        const auto& sub = *it;
        auto new_sub = (*f)(obj, sub.c_str(), args...);
        if (new_sub.size()) {
            if (res.empty()) {
                res.reserve(subs.size());
                res.insert(res.end(), subs.begin(), it);
            }
            res.push_back(std::move(new_sub));
        }
        else if (!res.empty()) {
            res.push_back(sub);
        }
    }
    return res;
}

Property*
PropertyLinkSub::CopyOnImportExternal(const std::map<std::string, std::string>& nameMap) const
{
    auto owner = dynamic_cast<const DocumentObject*>(getContainer());
    if (!owner || !owner->getDocument()) {
        return nullptr;
    }
    if (!_pcLinkSub || !_pcLinkSub->isAttachedToDocument()) {
        return nullptr;
    }

    auto subs =
        updateLinkSubs(_pcLinkSub, _cSubList, &tryImportSubName, owner->getDocument(), nameMap);
    auto linked = tryImport(owner->getDocument(), _pcLinkSub, nameMap);
    if (subs.empty() && linked == _pcLinkSub) {
        return nullptr;
    }

    PropertyLinkSub* p = new PropertyLinkSub();
    p->_pcLinkSub = linked;
    if (subs.empty()) {
        p->_cSubList = _cSubList;
    }
    else {
        p->_cSubList = std::move(subs);
    }
    return p;
}

Property* PropertyLinkSub::CopyOnLabelChange(App::DocumentObject* obj,
                                             const std::string& ref,
                                             const char* newLabel) const
{
    auto owner = dynamic_cast<const DocumentObject*>(getContainer());
    if (!owner || !owner->getDocument()) {
        return nullptr;
    }
    if (!_pcLinkSub || !_pcLinkSub->isAttachedToDocument()) {
        return nullptr;
    }

    auto subs = updateLinkSubs(_pcLinkSub, _cSubList, &updateLabelReference, obj, ref, newLabel);
    if (subs.empty()) {
        return nullptr;
    }

    PropertyLinkSub* p = new PropertyLinkSub();
    p->_pcLinkSub = _pcLinkSub;
    p->_cSubList = std::move(subs);
    return p;
}

Property* PropertyLinkSub::CopyOnLinkReplace(const App::DocumentObject* parent,
                                             App::DocumentObject* oldObj,
                                             App::DocumentObject* newObj) const
{
    auto res = tryReplaceLinkSubs(getContainer(), _pcLinkSub, parent, oldObj, newObj, _cSubList);
    if (res.first) {
        PropertyLinkSub* p = new PropertyLinkSub();
        p->_pcLinkSub = res.first;
        p->_cSubList = std::move(res.second);
        return p;
    }
    return nullptr;
}

Property* PropertyLinkSub::Copy() const
{
    PropertyLinkSub* p = new PropertyLinkSub();
    p->_pcLinkSub = _pcLinkSub;
    p->_cSubList = _cSubList;
    p->_ShadowSubList = _ShadowSubList;
    return p;
}

void PropertyLinkSub::Paste(const Property& from)
{
    if (!from.isDerivedFrom<PropertyLinkSub>()) {
        throw Base::TypeError("Incompatible property to paste to");
    }
    auto& link = static_cast<const PropertyLinkSub&>(from);
    setValue(link._pcLinkSub, link._cSubList, std::vector<ShadowSub>(link._ShadowSubList));
}

void PropertyLinkSub::getLinks(std::vector<App::DocumentObject*>& objs,
                               bool all,
                               std::vector<std::string>* subs,
                               bool newStyle) const
{
    if (all || _pcScope != LinkScope::Hidden) {
        if (_pcLinkSub && _pcLinkSub->isAttachedToDocument()) {
            objs.push_back(_pcLinkSub);
            if (subs) {
                *subs = getSubValues(newStyle);
            }
        }
    }
}

void PropertyLinkSub::getLinksTo(std::vector<App::ObjectIdentifier>& identifiers,
                                 App::DocumentObject* obj,
                                 const char* subname,
                                 bool all) const
{
    if (all || _pcScope != LinkScope::Hidden) {
        if (obj && obj == _pcLinkSub) {
            _getLinksTo(identifiers, obj, subname, _cSubList, _ShadowSubList);
        }
    }
}

void PropertyLinkSub::breakLink(App::DocumentObject* obj, bool clear)
{
    if (obj == _pcLinkSub || (clear && getContainer() == obj)) {
        setValue(nullptr);
    }
}

static App::DocumentObject*
adjustLinkSubs(App::PropertyLinkBase* prop,
               const std::set<App::DocumentObject*>& inList,
               App::DocumentObject* link,
               std::vector<std::string>& subs,
               std::map<App::DocumentObject*, std::vector<std::string>>* links = nullptr)
{
    App::DocumentObject* newLink = nullptr;
    for (auto& sub : subs) {
        size_t pos = sub.find('.');
        for (; pos != std::string::npos; pos = sub.find('.', pos + 1)) {
            auto sobj = link->getSubObject(sub.substr(0, pos + 1).c_str());
            if (!sobj
                || (!prop->testFlag(PropertyLinkBase::LinkAllowExternal)
                    && sobj->getDocument() != link->getDocument())) {
                pos = std::string::npos;
                break;
            }
            if (!newLink) {
                if (inList.count(sobj)) {
                    continue;
                }
                newLink = sobj;
                if (links) {
                    (*links)[sobj].push_back(sub.substr(pos + 1));
                }
                else {
                    sub = sub.substr(pos + 1);
                }
            }
            else if (links) {
                (*links)[sobj].push_back(sub.substr(pos + 1));
            }
            else if (sobj == newLink) {
                sub = sub.substr(pos + 1);
            }
            break;
        }
        if (pos == std::string::npos) {
            return nullptr;
        }
    }
    return newLink;
}

bool PropertyLinkSub::adjustLink(const std::set<App::DocumentObject*>& inList)
{
    if (_pcScope == LinkScope::Hidden) {
        return false;
    }
    if (!_pcLinkSub || !_pcLinkSub->isAttachedToDocument() || !inList.count(_pcLinkSub)) {
        return false;
    }
    auto subs = _cSubList;
    auto link = adjustLinkSubs(this, inList, _pcLinkSub, subs);
    if (link) {
        setValue(link, std::move(subs));
        return true;
    }
    return false;
}

//**************************************************************************
// PropertyLinkSubList
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE(App::PropertyLinkSubList, App::PropertyLinkBase)
TYPESYSTEM_SOURCE(App::PropertyLinkSubListChild, App::PropertyLinkSubList)
TYPESYSTEM_SOURCE(App::PropertyLinkSubListGlobal, App::PropertyLinkSubList)
TYPESYSTEM_SOURCE(App::PropertyLinkSubListHidden, App::PropertyLinkSubList)

//**************************************************************************
// Construction/Destruction


PropertyLinkSubList::PropertyLinkSubList() = default;

PropertyLinkSubList::~PropertyLinkSubList()
{
    // in case this property is dynamically removed

    // maintain backlinks
    if (!_lValueList.empty() && getContainer()
        && getContainer()->isDerivedFrom<App::DocumentObject>()) {
        App::DocumentObject* parent = static_cast<DocumentObject*>(getContainer());
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
            for (auto* obj : _lValueList) {
                if (obj) {
                    obj->_removeBackLink(parent);
                }
            }
        }
    }
}

void PropertyLinkSubList::setSyncSubObject(bool enable)
{
    _Flags.set((std::size_t)LinkSyncSubObject, enable);
}

void PropertyLinkSubList::verifyObject(App::DocumentObject* obj, App::DocumentObject* parent)
{
    if (obj) {
        if (!obj->isAttachedToDocument()) {
            throw Base::ValueError("PropertyLinkSubList: invalid document object");
        }
        if (!testFlag(LinkAllowExternal) && parent && parent->getDocument() != obj->getDocument()) {
            throw Base::ValueError("PropertyLinkSubList does not support external object");
        }
    }
}

void PropertyLinkSubList::setSize(int newSize)
{
    _lValueList.resize(newSize);
    _lSubList.resize(newSize);
    _ShadowSubList.resize(newSize);
}

int PropertyLinkSubList::getSize() const
{
    return static_cast<int>(_lValueList.size());
}

void PropertyLinkSubList::setValue(DocumentObject* lValue, const char* SubName)
{
    auto parent = Base::freecad_dynamic_cast<App::DocumentObject>(getContainer());
    verifyObject(lValue, parent);

    // maintain backlinks
    if (parent) {
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
            for (auto* obj : _lValueList) {
                if (obj) {
                    obj->_removeBackLink(parent);
                }
            }
            if (lValue) {
                lValue->_addBackLink(parent);
            }
        }
    }

    if (lValue) {
        aboutToSetValue();
        _lValueList.resize(1);
        _lValueList[0] = lValue;
        _lSubList.resize(1);
        _lSubList[0] = SubName;
    }
    else {
        aboutToSetValue();
        _lValueList.clear();
        _lSubList.clear();
    }
    updateElementReference(nullptr);
    checkLabelReferences(_lSubList);
    hasSetValue();
}

void PropertyLinkSubList::setValues(const std::vector<DocumentObject*>& lValue,
                                    const std::vector<const char*>& lSubNames)
{
    auto parent = Base::freecad_dynamic_cast<App::DocumentObject>(getContainer());
    for (auto obj : lValue) {
        verifyObject(obj, parent);
    }

    if (lValue.size() != lSubNames.size()) {
        throw Base::ValueError(
            "PropertyLinkSubList::setValues: size of subelements list != size of objects list");
    }

    // maintain backlinks.
    if (parent) {
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
            //_lValueList can contain items multiple times, but we trust the document
            // object to ensure that this works
            for (auto* obj : _lValueList) {
                if (obj) {
                    obj->_removeBackLink(parent);
                }
            }

            // maintain backlinks. lValue can contain items multiple times, but we trust the
            // document object to ensure that the backlink is only added once
            for (auto* obj : lValue) {
                if (obj) {
                    obj->_addBackLink(parent);
                }
            }
        }
    }

    aboutToSetValue();
    _lValueList = lValue;
    _lSubList.resize(lSubNames.size());
    int i = 0;
    for (std::vector<const char*>::const_iterator it = lSubNames.begin(); it != lSubNames.end();
         ++it, ++i) {
        if (*it) {
            _lSubList[i] = *it;
        }
    }
    updateElementReference(nullptr);
    checkLabelReferences(_lSubList);
    hasSetValue();
}

void PropertyLinkSubList::setValues(const std::vector<DocumentObject*>& lValue,
                                    const std::vector<std::string>& lSubNames,
                                    std::vector<ShadowSub>&& ShadowSubList)
{
    setValues(std::vector<DocumentObject*>(lValue),
              std::vector<std::string>(lSubNames),
              std::move(ShadowSubList));
}

void PropertyLinkSubList::setValues(std::vector<DocumentObject*>&& lValue,
                                    std::vector<std::string>&& lSubNames,
                                    std::vector<ShadowSub>&& ShadowSubList)
{
    auto parent = Base::freecad_dynamic_cast<App::DocumentObject>(getContainer());
    for (auto obj : lValue) {
        verifyObject(obj, parent);
    }
    if (lValue.size() != lSubNames.size()) {
        throw Base::ValueError(
            "PropertyLinkSubList::setValues: size of subelements list != size of objects list");
    }

    // maintain backlinks.
    if (parent) {
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
            //_lValueList can contain items multiple times, but we trust the document
            // object to ensure that this works
            for (auto* obj : _lValueList) {
                if (obj) {
                    obj->_removeBackLink(parent);
                }
            }

            // maintain backlinks. lValue can contain items multiple times, but we trust the
            // document object to ensure that the backlink is only added once
            for (auto* obj : lValue) {
                if (obj) {
                    obj->_addBackLink(parent);
                }
            }
        }
    }

    aboutToSetValue();
    _lValueList = std::move(lValue);
    _lSubList = std::move(lSubNames);
    if (ShadowSubList.size() == _lSubList.size()) {
        _ShadowSubList = std::move(ShadowSubList);
        onContainerRestored();  // re-register element references
    }
    else {
        updateElementReference(nullptr);
    }
    checkLabelReferences(_lSubList);
    hasSetValue();
}

void PropertyLinkSubList::setValue(DocumentObject* lValue, const std::vector<std::string>& SubList)
{
    auto parent = dynamic_cast<App::DocumentObject*>(getContainer());
    verifyObject(lValue, parent);

    // maintain backlinks.
    if (parent) {
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
            //_lValueList can contain items multiple times, but we trust the document
            // object to ensure that this works
            for (auto* obj : _lValueList) {
                if (obj) {
                    obj->_removeBackLink(parent);
                }
            }

            // maintain backlinks. lValue can contain items multiple times, but we trust the
            // document object to ensure that the backlink is only added once
            if (lValue) {
                lValue->_addBackLink(parent);
            }
        }
    }

    aboutToSetValue();
    std::size_t size = SubList.size();
    this->_lValueList.clear();
    this->_lSubList.clear();
    if (size == 0) {
        if (lValue) {
            this->_lValueList.push_back(lValue);
            this->_lSubList.emplace_back();
        }
    }
    else {
        this->_lSubList = SubList;
        this->_lValueList.insert(this->_lValueList.begin(), size, lValue);
    }
    updateElementReference(nullptr);
    checkLabelReferences(_lSubList);
    hasSetValue();
}

void PropertyLinkSubList::addValue(App::DocumentObject* obj,
                                   const std::vector<std::string>& subs,
                                   bool reset)
{
    auto parent = Base::freecad_dynamic_cast<App::DocumentObject>(getContainer());
    verifyObject(obj, parent);

    // maintain backlinks.
    if (parent) {
        // before accessing internals make sure the object is not about to be destroyed
        // otherwise the backlink contains dangling pointers
        if (!parent->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
            //_lValueList can contain items multiple times, but we trust the document
            // object to ensure that this works
            if (reset) {
                for (auto* value : _lValueList) {
                    if (value && value == obj) {
                        value->_removeBackLink(parent);
                    }
                }
            }

            // maintain backlinks. lValue can contain items multiple times, but we trust the
            // document object to ensure that the backlink is only added once
            if (obj) {
                obj->_addBackLink(parent);
            }
        }
    }

    std::vector<DocumentObject*> valueList;
    std::vector<std::string> subList;

    if (reset) {
        for (std::size_t i = 0; i < _lValueList.size(); i++) {
            if (_lValueList[i] != obj) {
                valueList.push_back(_lValueList[i]);
                subList.push_back(_lSubList[i]);
            }
        }
    }
    else {
        valueList = _lValueList;
        subList = _lSubList;
    }

    std::size_t size = subs.size();
    if (size == 0) {
        if (obj) {
            valueList.push_back(obj);
            subList.emplace_back();
        }
    }
    else if (obj) {
        subList.insert(subList.end(), subs.begin(), subs.end());
        valueList.insert(valueList.end(), size, obj);
    }

    aboutToSetValue();
    _lValueList = valueList;
    _lSubList = subList;
    updateElementReference(nullptr);
    checkLabelReferences(_lSubList);
    hasSetValue();
}

const string PropertyLinkSubList::getPyReprString() const
{
    assert(this->_lValueList.size() == this->_lSubList.size());

    if (this->_lValueList.empty()) {
        return std::string("None");
    }

    std::stringstream strm;
    strm << "[";
    for (std::size_t i = 0; i < this->_lSubList.size(); i++) {
        if (i > 0) {
            strm << ",(";
        }
        else {
            strm << "(";
        }
        App::DocumentObject* obj = this->_lValueList[i];
        if (obj) {
            strm << "App.getDocument('" << obj->getDocument()->getName() << "').getObject('"
                 << obj->getNameInDocument() << "')";
        }
        else {
            strm << "None";
        }
        strm << ",";
        strm << "'" << this->_lSubList[i] << "'";
        strm << ")";
    }
    strm << "]";
    return strm.str();
}

DocumentObject* PropertyLinkSubList::getValue() const
{
    App::DocumentObject* ret = nullptr;
    // FIXME: cache this to avoid iterating each time, to improve speed
    for (auto i : this->_lValueList) {
        if (!ret) {
            ret = i;
        }
        if (ret != i) {
            return nullptr;
        }
    }
    return ret;
}

int PropertyLinkSubList::removeValue(App::DocumentObject* lValue)
{
    assert(this->_lValueList.size() == this->_lSubList.size());

    std::size_t num = std::count(this->_lValueList.begin(), this->_lValueList.end(), lValue);
    if (num == 0) {
        return 0;
    }

    std::vector<DocumentObject*> links;
    std::vector<std::string> subs;
    links.reserve(this->_lValueList.size() - num);
    subs.reserve(this->_lSubList.size() - num);

    for (std::size_t i = 0; i < this->_lValueList.size(); ++i) {
        if (this->_lValueList[i] != lValue) {
            links.push_back(this->_lValueList[i]);
            subs.push_back(this->_lSubList[i]);
        }
    }

    setValues(links, subs);
    return static_cast<int>(num);
}

void PropertyLinkSubList::setSubListValues(const std::vector<PropertyLinkSubList::SubSet>& values)
{
    std::vector<DocumentObject*> links;
    std::vector<std::string> subs;
    for (std::vector<PropertyLinkSubList::SubSet>::const_iterator it = values.begin();
         it != values.end();
         ++it) {
        if (it->second.empty()) {
            links.push_back(it->first);
            subs.emplace_back();
            continue;
        }
        for (std::vector<std::string>::const_iterator jt = it->second.begin();
             jt != it->second.end();
             ++jt) {
            links.push_back(it->first);
            subs.push_back(*jt);
        }
    }
    setValues(links, subs);
}

std::vector<PropertyLinkSubList::SubSet> PropertyLinkSubList::getSubListValues(bool newStyle) const
{
    std::vector<PropertyLinkSubList::SubSet> values;
    if (_lValueList.size() != _lSubList.size()) {
        throw Base::ValueError("PropertyLinkSubList::getSubListValues: size of subelements list != "
                               "size of objects list");
    }

    assert(_ShadowSubList.size() == _lSubList.size());

    for (std::size_t i = 0; i < _lValueList.size(); i++) {
        App::DocumentObject* link = _lValueList[i];
        std::string sub;
        if (newStyle && !_ShadowSubList[i].newName.empty()) {
            sub = _ShadowSubList[i].newName;
        }
        else if (!newStyle && !_ShadowSubList[i].oldName.empty()) {
            sub = _ShadowSubList[i].oldName;
        }
        else {
            sub = _lSubList[i];
        }
        if (values.empty() || values.back().first != link) {
            // new object started, start a new subset.
            values.emplace_back(link, std::vector<std::string>());
        }
        values.back().second.push_back(sub);
    }
    return values;
}

PyObject* PropertyLinkSubList::getPyObject()
{
    std::vector<SubSet> subLists = getSubListValues();
    std::size_t count = subLists.size();
#if 0  // FIXME: Should switch to tuple
    Py::Tuple sequence(count);
#else
    Py::List sequence(count);
#endif
    for (std::size_t i = 0; i < count; i++) {
        Py::Tuple tup(2);
        tup[0] = Py::asObject(subLists[i].first->getPyObject());

        const std::vector<std::string>& sub = subLists[i].second;
        Py::Tuple items(sub.size());
        for (std::size_t j = 0; j < sub.size(); j++) {
            items[j] = Py::String(sub[j]);
        }

        tup[1] = items;
        sequence[i] = tup;
    }

    return Py::new_reference_to(sequence);
}

void PropertyLinkSubList::setPyObject(PyObject* value)
{
    try {  // try PropertyLinkSub syntax
        PropertyLinkSub dummy;
        dummy.setPyObject(value);
        this->setValue(dummy.getValue(), dummy.getSubValues());
        return;
    }
    catch (...) {
    }
    try {
        // try PropertyLinkList syntax
        PropertyLinkList dummy;
        dummy.setPyObject(value);
        const auto& values = dummy.getValues();
        std::vector<std::string> subs(values.size());
        this->setValues(values, subs);
        return;
    }
    catch (...) {
    }

    static const char* errMsg =
        "Expects sequence of items of type DocObj, (DocObj,SubName), or (DocObj, (SubName,...))";

    if (!PyTuple_Check(value) && !PyList_Check(value)) {
        throw Base::TypeError(errMsg);
    }

    Py::Sequence list(value);
    Py::Sequence::size_type size = list.size();

    std::vector<DocumentObject*> values;
    values.reserve(size);
    std::vector<std::string> SubNames;
    SubNames.reserve(size);
    for (Py::Sequence::size_type i = 0; i < size; i++) {
        Py::Object item = list[i];
        if ((item.isTuple() || item.isSequence()) && PySequence_Size(*item) == 2) {
            Py::Sequence seq(item);
            if (PyObject_TypeCheck(seq[0].ptr(), &(DocumentObjectPy::Type))) {
                auto obj = static_cast<DocumentObjectPy*>(seq[0].ptr())->getDocumentObjectPtr();
                PropertyString propString;
                if (seq[1].isString()) {
                    values.push_back(obj);
                    propString.setPyObject(seq[1].ptr());
                    SubNames.emplace_back(propString.getValue());
                }
                else if (seq[1].isSequence()) {
                    Py::Sequence list(seq[1]);
                    for (Py::Sequence::iterator it = list.begin(); it != list.end(); ++it) {
                        if (!(*it).isString()) {
                            throw Base::TypeError(errMsg);
                        }
                        values.push_back(obj);
                        propString.setPyObject((*it).ptr());
                        SubNames.emplace_back(propString.getValue());
                    }
                }
                else {
                    throw Base::TypeError(errMsg);
                }
            }
        }
        else if (PyObject_TypeCheck(*item, &(DocumentObjectPy::Type))) {
            DocumentObjectPy* pcObj;
            pcObj = static_cast<DocumentObjectPy*>(*item);
            values.push_back(pcObj->getDocumentObjectPtr());
            SubNames.emplace_back();
        }
        else {
            throw Base::TypeError(errMsg);
        }
    }
    setValues(values, SubNames);
}

void PropertyLinkSubList::afterRestore()
{
    assert(_lSubList.size() == _ShadowSubList.size());
    if (!testFlag(LinkRestoreLabel)) {
        return;
    }
    setFlag(LinkRestoreLabel, false);
    for (size_t i = 0; i < _lSubList.size(); ++i) {
        restoreLabelReference(_lValueList[i], _lSubList[i], &_ShadowSubList[i]);
    }
}

void PropertyLinkSubList::onContainerRestored()
{
    unregisterElementReference();
    for (size_t i = 0; i < _lSubList.size(); ++i) {
        _registerElementReference(_lValueList[i], _lSubList[i], _ShadowSubList[i]);
    }
}

void PropertyLinkSubList::updateElementReference(DocumentObject* feature, bool reverse, bool notify)
{
    if (!feature) {
        _ShadowSubList.clear();
        unregisterElementReference();
    }
    _ShadowSubList.resize(_lSubList.size());
    auto owner = freecad_dynamic_cast<DocumentObject>(getContainer());
    if (owner && owner->isRestoring()) {
        return;
    }
    int i = 0;
    bool touched = false;
    for (auto& sub : _lSubList) {
        auto obj = _lValueList[i];
        if (_updateElementReference(feature,
                                    obj,
                                    sub,
                                    _ShadowSubList[i++],
                                    reverse,
                                    notify && !touched)) {
            touched = true;
        }
    }
    if (!touched) {
        return;
    }

    std::vector<int> mapped;
    mapped.reserve(_mapped.size());
    for (int idx : _mapped) {
        if (idx < (int)_lSubList.size()) {
            if (!_ShadowSubList[idx].newName.empty()) {
                _lSubList[idx] = _ShadowSubList[idx].newName;
            }
            else {
                mapped.push_back(idx);
            }
        }
    }
    _mapped.swap(mapped);
    if (owner && feature) {
        owner->onUpdateElementReference(this);
    }
    if (notify) {
        hasSetValue();
    }
}

bool PropertyLinkSubList::referenceChanged() const
{
    return !_mapped.empty();
}

void PropertyLinkSubList::Save(Base::Writer& writer) const
{
    assert(_lSubList.size() == _ShadowSubList.size());

    int count = 0;
    for (auto obj : _lValueList) {
        if (obj && obj->isAttachedToDocument()) {
            ++count;
        }
    }
    writer.Stream() << writer.ind() << "<LinkSubList count=\"" << count << "\">" << endl;
    writer.incInd();
    auto owner = dynamic_cast<DocumentObject*>(getContainer());
    bool exporting = owner && owner->isExporting();
    for (int i = 0; i < getSize(); i++) {
        auto obj = _lValueList[i];
        if (!obj || !obj->isAttachedToDocument()) {
            continue;
        }
        const auto& shadow = _ShadowSubList[i];
        // shadow.oldName stores the old style element name. For backward
        // compatibility reason, we shall store the old name into attribute
        // 'value' whenever possible.
        const auto& sub = shadow.oldName.empty() ? _lSubList[i] : shadow.oldName;

        writer.Stream() << writer.ind() << "<Link obj=\"" << obj->getExportName() << "\" sub=\"";
        if (exporting) {
            std::string exportName;
            writer.Stream() << encodeAttribute(exportSubName(exportName, obj, sub.c_str()));
            if (!shadow.oldName.empty() && _lSubList[i] == shadow.newName) {
                writer.Stream() << "\" " ATTR_MAPPED "=\"1";
            }
        }
        else {
            writer.Stream() << encodeAttribute(sub);
            if (!_lSubList[i].empty()) {
                if (sub != _lSubList[i]) {
                    // Stores the actual value that is shadowed. For new version FC,
                    // we will restore this shadowed value instead.
                    writer.Stream() << "\" " ATTR_SHADOWED "=\"" << encodeAttribute(_lSubList[i]);
                }
                else if (!shadow.newName.empty()) {
                    // Here means the user set value is old style element name.
                    // We shall then store the shadow somewhere else.
                    writer.Stream() << "\" " ATTR_SHADOW "=\"" << encodeAttribute(shadow.newName);
                }
            }
        }
        writer.Stream() << "\"/>" << endl;
    }

    writer.decInd();
    writer.Stream() << writer.ind() << "</LinkSubList>" << endl;
}

void PropertyLinkSubList::Restore(Base::XMLReader& reader)
{
    // read my element
    reader.readElement("LinkSubList");
    // get the value of my attribute
    int count = reader.getAttributeAsInteger("count");

    std::vector<DocumentObject*> values;
    values.reserve(count);
    std::vector<std::string> SubNames;
    SubNames.reserve(count);
    std::vector<ShadowSub> shadows;
    shadows.reserve(count);
    DocumentObject* father = dynamic_cast<DocumentObject*>(getContainer());
    App::Document* document = father ? father->getDocument() : nullptr;
    std::vector<int> mapped;
    bool restoreLabel = false;
    for (int i = 0; i < count; i++) {
        reader.readElement("Link");
        std::string name = reader.getName(reader.getAttribute("obj"));
        // In order to do copy/paste it must be allowed to have defined some
        // referenced objects in XML which do not exist anymore in the new
        // document. Thus, we should silently ignore this.
        // Property not in an object!
        DocumentObject* child = document ? document->getObject(name.c_str()) : nullptr;
        if (child) {
            values.push_back(child);
            shadows.emplace_back();
            auto& shadow = shadows.back();
            shadow.oldName = importSubName(reader, reader.getAttribute("sub"), restoreLabel);
            if (reader.hasAttribute(ATTR_SHADOWED) && !IGNORE_SHADOW) {
                shadow.newName =
                    importSubName(reader, reader.getAttribute(ATTR_SHADOWED), restoreLabel);
                SubNames.push_back(shadow.newName);
            }
            else {
                SubNames.push_back(shadow.oldName);
                if (reader.hasAttribute(ATTR_SHADOW) && !IGNORE_SHADOW) {
                    shadow.newName =
                        importSubName(reader, reader.getAttribute(ATTR_SHADOW), restoreLabel);
                }
            }
            if (reader.hasAttribute(ATTR_MAPPED)) {
                mapped.push_back(i);
            }
        }
        else if (reader.isVerbose()) {
            Base::Console().Warning("Lost link to '%s' while loading, maybe "
                                    "an object was not loaded correctly\n",
                                    name.c_str());
        }
    }
    setFlag(LinkRestoreLabel, restoreLabel);

    reader.readEndElement("LinkSubList");

    // assignment
    setValues(values, SubNames, std::move(shadows));
    _mapped.swap(mapped);
}

bool PropertyLinkSubList::upgrade(Base::XMLReader& reader, const char* typeName)
{
    Base::Type type = Base::Type::fromName(typeName);
    if (type.isDerivedFrom(PropertyLink::getClassTypeId())) {
        PropertyLink prop;
        prop.setContainer(getContainer());
        prop.Restore(reader);
        setValue(prop.getValue());
        return true;
    }
    else if (type.isDerivedFrom(PropertyLinkList::getClassTypeId())) {
        PropertyLinkList prop;
        prop.setContainer(getContainer());
        prop.Restore(reader);
        std::vector<std::string> subnames;
        subnames.resize(prop.getSize());
        setValues(prop.getValues(), subnames);
        return true;
    }
    else if (type.isDerivedFrom(PropertyLinkSub::getClassTypeId())) {
        PropertyLinkSub prop;
        prop.setContainer(getContainer());
        prop.Restore(reader);
        setValue(prop.getValue(), prop.getSubValues());
        return true;
    }

    return false;
}

Property*
PropertyLinkSubList::CopyOnImportExternal(const std::map<std::string, std::string>& nameMap) const
{
    auto owner = dynamic_cast<const DocumentObject*>(getContainer());
    if (!owner || !owner->getDocument() || _lValueList.size() != _lSubList.size()) {
        return nullptr;
    }
    std::vector<App::DocumentObject*> values;
    std::vector<std::string> subs;
    auto itSub = _lSubList.begin();
    for (auto itValue = _lValueList.begin(); itValue != _lValueList.end(); ++itValue, ++itSub) {
        auto value = *itValue;
        const auto& sub = *itSub;
        if (!value || !value->isAttachedToDocument()) {
            if (!values.empty()) {
                values.push_back(value);
                subs.push_back(sub);
            }
            continue;
        }
        auto linked = tryImport(owner->getDocument(), value, nameMap);
        auto new_sub = tryImportSubName(value, sub.c_str(), owner->getDocument(), nameMap);
        if (linked != value || !new_sub.empty()) {
            if (values.empty()) {
                values.reserve(_lValueList.size());
                values.insert(values.end(), _lValueList.begin(), itValue);
                subs.reserve(_lSubList.size());
                subs.insert(subs.end(), _lSubList.begin(), itSub);
            }
            values.push_back(linked);
            subs.push_back(std::move(new_sub));
        }
        else if (!values.empty()) {
            values.push_back(linked);
            subs.push_back(sub);
        }
    }
    if (values.empty()) {
        return nullptr;
    }
    std::unique_ptr<PropertyLinkSubList> p(new PropertyLinkSubList);
    p->_lValueList = std::move(values);
    p->_lSubList = std::move(subs);
    return p.release();
}

Property* PropertyLinkSubList::CopyOnLabelChange(App::DocumentObject* obj,
                                                 const std::string& ref,
                                                 const char* newLabel) const
{
    auto owner = dynamic_cast<const DocumentObject*>(getContainer());
    if (!owner || !owner->getDocument()) {
        return nullptr;
    }
    std::vector<App::DocumentObject*> values;
    std::vector<std::string> subs;
    auto itSub = _lSubList.begin();
    for (auto itValue = _lValueList.begin(); itValue != _lValueList.end(); ++itValue, ++itSub) {
        auto value = *itValue;
        const auto& sub = *itSub;
        if (!value || !value->isAttachedToDocument()) {
            if (!values.empty()) {
                values.push_back(value);
                subs.push_back(sub);
            }
            continue;
        }
        auto new_sub = updateLabelReference(value, sub.c_str(), obj, ref, newLabel);
        if (!new_sub.empty()) {
            if (values.empty()) {
                values.reserve(_lValueList.size());
                values.insert(values.end(), _lValueList.begin(), itValue);
                subs.reserve(_lSubList.size());
                subs.insert(subs.end(), _lSubList.begin(), itSub);
            }
            values.push_back(value);
            subs.push_back(std::move(new_sub));
        }
        else if (!values.empty()) {
            values.push_back(value);
            subs.push_back(sub);
        }
    }
    if (values.empty()) {
        return nullptr;
    }
    std::unique_ptr<PropertyLinkSubList> p(new PropertyLinkSubList);
    p->_lValueList = std::move(values);
    p->_lSubList = std::move(subs);
    return p.release();
}

Property* PropertyLinkSubList::CopyOnLinkReplace(const App::DocumentObject* parent,
                                                 App::DocumentObject* oldObj,
                                                 App::DocumentObject* newObj) const
{
    std::vector<App::DocumentObject*> values;
    std::vector<std::string> subs;
    auto itSub = _lSubList.begin();
    std::vector<size_t> positions;
    for (auto itValue = _lValueList.begin(); itValue != _lValueList.end(); ++itValue, ++itSub) {
        auto value = *itValue;
        const auto& sub = *itSub;
        if (!value || !value->isAttachedToDocument()) {
            if (!values.empty()) {
                values.push_back(value);
                subs.push_back(sub);
            }
            continue;
        }
        auto res = tryReplaceLink(getContainer(), value, parent, oldObj, newObj, sub.c_str());
        if (res.first) {
            if (values.empty()) {
                values.reserve(_lValueList.size());
                values.insert(values.end(), _lValueList.begin(), itValue);
                subs.reserve(_lSubList.size());
                subs.insert(subs.end(), _lSubList.begin(), itSub);
            }
            if (res.first == newObj) {
                // check for duplication
                auto itS = subs.begin();
                for (auto itV = values.begin(); itV != values.end();) {
                    if (*itV == res.first && *itS == res.second) {
                        itV = values.erase(itV);
                        itS = subs.erase(itS);
                    }
                    else {
                        ++itV;
                        ++itS;
                    }
                }
                positions.push_back(values.size());
            }
            values.push_back(res.first);
            subs.push_back(std::move(res.second));
        }
        else if (!values.empty()) {
            bool duplicate = false;
            if (value == newObj) {
                for (auto pos : positions) {
                    if (sub == subs[pos]) {
                        duplicate = true;
                        break;
                    }
                }
            }
            if (!duplicate) {
                values.push_back(value);
                subs.push_back(sub);
            }
        }
    }
    if (values.empty()) {
        return nullptr;
    }
    std::unique_ptr<PropertyLinkSubList> p(new PropertyLinkSubList);
    p->_lValueList = std::move(values);
    p->_lSubList = std::move(subs);
    return p.release();
}

Property* PropertyLinkSubList::Copy() const
{
    PropertyLinkSubList* p = new PropertyLinkSubList();
    p->_lValueList = _lValueList;
    p->_lSubList = _lSubList;
    p->_ShadowSubList = _ShadowSubList;
    return p;
}

void PropertyLinkSubList::Paste(const Property& from)
{
    if (!from.isDerivedFrom<PropertyLinkSubList>()) {
        throw Base::TypeError("Incompatible property to paste to");
    }
    auto& link = static_cast<const PropertyLinkSubList&>(from);
    setValues(link._lValueList, link._lSubList, std::vector<ShadowSub>(link._ShadowSubList));
}

unsigned int PropertyLinkSubList::getMemSize() const
{
    unsigned int size =
        static_cast<unsigned int>(_lValueList.size() * sizeof(App::DocumentObject*));
    for (int i = 0; i < getSize(); i++) {
        size += _lSubList[i].size();
    }
    return size;
}

std::vector<std::string> PropertyLinkSubList::getSubValues(bool newStyle) const
{
    assert(_lSubList.size() == _ShadowSubList.size());
    std::vector<std::string> ret;
    ret.reserve(_ShadowSubList.size());
    std::string tmp;
    for (size_t i = 0; i < _ShadowSubList.size(); ++i) {
        ret.push_back(getSubNameWithStyle(_lSubList[i], _ShadowSubList[i], newStyle, tmp));
    }
    return ret;
}

void PropertyLinkSubList::getLinks(std::vector<App::DocumentObject*>& objs,
                                   bool all,
                                   std::vector<std::string>* subs,
                                   bool newStyle) const
{
    if (all || _pcScope != LinkScope::Hidden) {
        objs.reserve(objs.size() + _lValueList.size());
        for (auto obj : _lValueList) {
            if (obj && obj->isAttachedToDocument()) {
                objs.push_back(obj);
            }
        }
        if (subs) {
            auto _subs = getSubValues(newStyle);
            subs->reserve(subs->size() + _subs.size());
            std::move(_subs.begin(), _subs.end(), std::back_inserter(*subs));
        }
    }
}

void PropertyLinkSubList::getLinksTo(std::vector<App::ObjectIdentifier>& identifiers,
                                     App::DocumentObject* obj,
                                     const char* subname,
                                     bool all) const
{
    if (!obj || (!all && _pcScope == LinkScope::Hidden)) {
        return;
    }
    App::SubObjectT objT(obj, subname);
    auto subObject = objT.getSubObject();
    auto subElement = objT.getOldElementName();

    int i = -1;
    for (const auto& docObj : _lValueList) {
        ++i;
        if (docObj != obj) {
            continue;
        }
        // If we don't specify a subname we looking for all; or if the subname is in our
        // property, add this entry to our result
        if (!subname || (i < (int)_lSubList.size() && subname == _lSubList[i])) {
            identifiers.emplace_back(*this, i);
            continue;
        }
        // If we couldn't find any subobjects or this object's index is in our list, ignore it
        if (!subObject || i < (int)_lSubList.size()) {
            continue;
        }
        App::SubObjectT sobjT(obj, _lSubList[i].c_str());
        if (sobjT.getSubObject() == subObject && sobjT.getOldElementName() == subElement) {
            identifiers.emplace_back(*this);
            continue;
        }
        if (i < (int)_ShadowSubList.size()) {
            const auto& shadow = _ShadowSubList[i];
            App::SubObjectT sobjT(obj,
                                  shadow.newName.empty() ? shadow.oldName.c_str()
                                                         : shadow.newName.c_str());
            if (sobjT.getSubObject() == subObject && sobjT.getOldElementName() == subElement) {
                identifiers.emplace_back(*this);
                continue;
            }
        }
    }
}

void PropertyLinkSubList::breakLink(App::DocumentObject* obj, bool clear)
{
    std::vector<DocumentObject*> values;
    std::vector<std::string> subs;

    if (clear && getContainer() == obj) {
        setValues(values, subs);
        return;
    }
    assert(_lValueList.size() == _lSubList.size());

    values.reserve(_lValueList.size());
    subs.reserve(_lSubList.size());

    int i = -1;
    for (auto o : _lValueList) {
        ++i;
        if (o == obj) {
            continue;
        }
        values.push_back(o);
        subs.push_back(_lSubList[i]);
    }
    if (values.size() != _lValueList.size()) {
        setValues(values, subs);
    }
}

bool PropertyLinkSubList::adjustLink(const std::set<App::DocumentObject*>& inList)
{
    if (_pcScope == LinkScope::Hidden) {
        return false;
    }
    auto subs = _lSubList;
    auto links = _lValueList;
    int idx = -1;
    bool touched = false;
    for (std::string& sub : subs) {
        ++idx;
        auto& link = links[idx];
        if (!link || !link->isAttachedToDocument() || !inList.count(link)) {
            continue;
        }
        touched = true;
        size_t pos = sub.find('.');
        for (; pos != std::string::npos; pos = sub.find('.', pos + 1)) {
            auto sobj = link->getSubObject(sub.substr(0, pos + 1).c_str());
            if (!sobj || sobj->getDocument() != link->getDocument()) {
                pos = std::string::npos;
                break;
            }
            if (!inList.count(sobj)) {
                link = sobj;
                sub = sub.substr(pos + 1);
                break;
            }
        }
        if (pos == std::string::npos) {
            return false;
        }
    }
    if (touched) {
        setValues(links, subs);
    }
    return touched;
}

//**************************************************************************
// DocInfo
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

// Key on absolute path.
// Because of possible symbolic links, multiple entry may refer to the same
// file. We used to rely on QFileInfo::canonicalFilePath to resolve it, but
// has now been changed to simply use the absoluteFilePath(), and rely on user
// to be aware of possible duplicated file location. The reason being that
// some user (especially Linux user) use symlink to organize file tree.
using DocInfoMap = std::map<QString, DocInfoPtr>;
DocInfoMap _DocInfoMap;

class App::DocInfo: public std::enable_shared_from_this<App::DocInfo>
{
public:
    using Connection = boost::signals2::scoped_connection;
    Connection connFinishRestoreDocument;
    Connection connPendingReloadDocument;
    Connection connDeleteDocument;
    Connection connSaveDocument;
    Connection connDeletedObject;

    DocInfoMap::iterator myPos;
    std::string myPath;
    App::Document* pcDoc {nullptr};
    std::set<PropertyXLink*> links;

    static std::string getDocPath(const char* filename,
                                  App::Document* pDoc,
                                  bool relative,
                                  QString* fullPath = nullptr)
    {
        bool absolute;
        // The path could be an URI, in that case
        // TODO: build a far much more resilient approach to test for an URI
        QString path = QString::fromUtf8(filename);
        if (path.startsWith(QLatin1String("https://"))) {
            // We do have an URI
            if (fullPath) {
                *fullPath = path;
            }
            return std::string(filename);
        }

        // make sure the filename is absolute path
        path = QDir::cleanPath(path);
        if ((absolute = QFileInfo(path).isAbsolute())) {
            if (fullPath) {
                *fullPath = path;
            }
            if (!relative) {
                return std::string(path.toUtf8().constData());
            }
        }

        const char* docPath = pDoc->getFileName();
        if (!docPath || *docPath == 0) {
            throw Base::RuntimeError("Owner document not saved");
        }

        QDir docDir(QFileInfo(QString::fromUtf8(docPath)).absoluteDir());
        if (!absolute) {
            path = QDir::cleanPath(docDir.absoluteFilePath(path));
            if (fullPath) {
                *fullPath = path;
            }
        }

        if (relative) {
            return std::string(docDir.relativeFilePath(path).toUtf8().constData());
        }
        else {
            return std::string(path.toUtf8().constData());
        }
    }

    static DocInfoPtr
    get(const char* filename, App::Document* pDoc, PropertyXLink* l, const char* objName)
    {
        QString path;
        l->filePath = getDocPath(filename, pDoc, true, &path);

        FC_LOG("finding doc " << filename);

        auto it = _DocInfoMap.find(path);
        DocInfoPtr info;
        if (it != _DocInfoMap.end()) {
            info = it->second;
            if (!info->pcDoc) {
                QString fullpath(info->getFullPath());
                if (fullpath.size()
                    && App::GetApplication().addPendingDocument(
                           fullpath.toUtf8().constData(),
                           objName,
                           l->testFlag(PropertyLinkBase::LinkAllowPartial))
                        == 0) {
                    for (App::Document* doc : App::GetApplication().getDocuments()) {
                        if (getFullPath(doc->getFileName()) == fullpath) {
                            info->attach(doc);
                            break;
                        }
                    }
                }
            }
        }
        else {
            info = std::make_shared<DocInfo>();
            auto ret = _DocInfoMap.insert(std::make_pair(path, info));
            info->init(ret.first, objName, l);
        }

        if (info->pcDoc) {
            // make sure to attach only external object
            auto owner = Base::freecad_dynamic_cast<DocumentObject>(l->getContainer());
            if (owner && owner->getDocument() == info->pcDoc) {
                return info;
            }
        }

        info->links.insert(l);
        return info;
    }

    static QString getFullPath(const char* p)
    {
        QString path = QString::fromUtf8(p);
        if (path.isEmpty()) {
            return path;
        }

        if (path.startsWith(QLatin1String("https://"))) {
            return path;
        }
        else {
            return QFileInfo(path).absoluteFilePath();
        }
    }

    QString getFullPath() const
    {
        QString path = myPos->first;
        if (path.startsWith(QLatin1String("https://"))) {
            return path;
        }
        else {
            return QFileInfo(myPos->first).absoluteFilePath();
        }
    }

    const char* filePath() const
    {
        return myPath.c_str();
    }

    void deinit()
    {
        FC_LOG("deinit " << (pcDoc ? pcDoc->getName() : filePath()));
        assert(links.empty());
        connFinishRestoreDocument.disconnect();
        connPendingReloadDocument.disconnect();
        connDeleteDocument.disconnect();
        connSaveDocument.disconnect();
        connDeletedObject.disconnect();

        auto me = shared_from_this();
        _DocInfoMap.erase(myPos);
        myPos = _DocInfoMap.end();
        myPath.clear();
        pcDoc = nullptr;
    }

    void init(DocInfoMap::iterator pos, const char* objName, PropertyXLink* l)
    {
        myPos = pos;
        myPath = myPos->first.toUtf8().constData();
        App::Application& app = App::GetApplication();
        // NOLINTBEGIN
        connFinishRestoreDocument = app.signalFinishRestoreDocument.connect(
            std::bind(&DocInfo::slotFinishRestoreDocument, this, sp::_1));
        connPendingReloadDocument = app.signalPendingReloadDocument.connect(
            std::bind(&DocInfo::slotFinishRestoreDocument, this, sp::_1));
        connDeleteDocument =
            app.signalDeleteDocument.connect(std::bind(&DocInfo::slotDeleteDocument, this, sp::_1));
        connSaveDocument =
            app.signalSaveDocument.connect(std::bind(&DocInfo::slotSaveDocument, this, sp::_1));
        // NOLINTEND

        QString fullpath(getFullPath());
        if (fullpath.isEmpty()) {
            FC_ERR("document not found " << filePath());
        }
        else {
            for (App::Document* doc : App::GetApplication().getDocuments()) {
                if (getFullPath(doc->getFileName()) == fullpath) {
                    if (doc->testStatus(App::Document::PartialDoc) && !doc->getObject(objName)) {
                        break;
                    }
                    attach(doc);
                    return;
                }
            }
            FC_LOG("document pending " << filePath());
            app.addPendingDocument(fullpath.toUtf8().constData(),
                                   objName,
                                   l->testFlag(PropertyLinkBase::LinkAllowPartial));
        }
    }

    void attach(Document* doc)
    {
        assert(!pcDoc);
        pcDoc = doc;
        FC_LOG("attaching " << doc->getName() << ", " << doc->getFileName());
        std::map<App::PropertyLinkBase*, std::vector<App::PropertyXLink*>> parentLinks;
        for (auto it = links.begin(), itNext = it; it != links.end(); it = itNext) {
            ++itNext;
            auto link = *it;
            if (link->_pcLink) {
                continue;
            }
            if (link->parentProp) {
                parentLinks[link->parentProp].push_back(link);
                continue;
            }
            auto obj = doc->getObject(link->objectName.c_str());
            if (obj) {
                link->restoreLink(obj);
            }
            else if (doc->testStatus(App::Document::PartialDoc)) {
                App::GetApplication().addPendingDocument(doc->FileName.getValue(),
                                                         link->objectName.c_str(),
                                                         false);
                FC_WARN("reloading partial document '" << doc->FileName.getValue()
                                                       << "' due to object " << link->objectName);
            }
            else {
                FC_WARN("object '" << link->objectName << "' not found in document '"
                                   << doc->getName() << "'");
            }
        }
        for (auto& v : parentLinks) {
            v.first->setFlag(PropertyLinkBase::LinkRestoring);
            v.first->aboutToSetValue();
            for (auto link : v.second) {
                auto obj = doc->getObject(link->objectName.c_str());
                if (obj) {
                    link->restoreLink(obj);
                }
                else if (doc->testStatus(App::Document::PartialDoc)) {
                    App::GetApplication().addPendingDocument(doc->FileName.getValue(),
                                                             link->objectName.c_str(),
                                                             false);
                    FC_WARN("reloading partial document '"
                            << doc->FileName.getValue() << "' due to object " << link->objectName);
                }
                else {
                    FC_WARN("object '" << link->objectName << "' not found in document '"
                                       << doc->getName() << "'");
                }
            }
            v.first->hasSetValue();
            v.first->setFlag(PropertyLinkBase::LinkRestoring, false);
        }
    }

    void remove(PropertyXLink* l)
    {
        auto it = links.find(l);
        if (it != links.end()) {
            links.erase(it);
            if (links.empty()) {
                deinit();
            }
        }
    }

    static void restoreDocument(const App::Document& doc)
    {
        auto it = _DocInfoMap.find(getFullPath(doc.FileName.getValue()));
        if (it == _DocInfoMap.end()) {
            return;
        }
        it->second->slotFinishRestoreDocument(doc);
    }

    void slotFinishRestoreDocument(const App::Document& doc)
    {
        if (pcDoc) {
            return;
        }
        QString fullpath(getFullPath());
        if (!fullpath.isEmpty() && getFullPath(doc.getFileName()) == fullpath) {
            attach(const_cast<App::Document*>(&doc));
        }
    }

    void slotSaveDocument(const App::Document& doc)
    {
        if (!pcDoc) {
            slotFinishRestoreDocument(doc);
            return;
        }
        if (&doc != pcDoc) {
            return;
        }

        QFileInfo info(myPos->first);
        QString path(info.absoluteFilePath());
        const char* filename = doc.getFileName();
        QString docPath(getFullPath(filename));

        if (path.isEmpty() || path != docPath) {
            FC_LOG("document '" << doc.getName() << "' path changed");
            auto me = shared_from_this();
            auto ret = _DocInfoMap.insert(std::make_pair(docPath, me));
            if (!ret.second) {
                // is that even possible?
                FC_WARN("document '" << doc.getName() << "' path exists, detach");
                slotDeleteDocument(doc);
                return;
            }
            _DocInfoMap.erase(myPos);
            myPos = ret.first;

            std::set<PropertyXLink*> tmp;
            tmp.swap(links);
            for (auto link : tmp) {
                auto owner = static_cast<DocumentObject*>(link->getContainer());
                // adjust file path for each PropertyXLink
                DocInfo::get(filename, owner->getDocument(), link, link->objectName.c_str());
            }
        }

        // time stamp changed, touch the linking document.
        std::set<Document*> docs;
        for (auto link : links) {
            auto linkdoc = static_cast<DocumentObject*>(link->getContainer())->getDocument();
            auto ret = docs.insert(linkdoc);
            if (ret.second) {
                // This will signal the Gui::Document to call setModified();
                FC_LOG("touch document " << linkdoc->getName() << " on time stamp change of "
                                         << link->getFullName());
                linkdoc->Comment.touch();
            }
        }
    }

    void slotDeleteDocument(const App::Document& doc)
    {
        for (auto it = links.begin(), itNext = it; it != links.end(); it = itNext) {
            ++itNext;
            auto link = *it;
            auto obj = dynamic_cast<DocumentObject*>(link->getContainer());
            if (obj && obj->getDocument() == &doc) {
                links.erase(it);
                // must call unlink here, so that PropertyLink::resetLink can
                // remove back link before the owner object is marked as being
                // destroyed
                link->unlink();
            }
        }
        if (links.empty()) {
            deinit();
            return;
        }
        if (pcDoc != &doc) {
            return;
        }
        std::map<App::PropertyLinkBase*, std::vector<App::PropertyXLink*>> parentLinks;
        for (auto link : links) {
            link->setFlag(PropertyLinkBase::LinkDetached);
            if (link->parentProp) {
                parentLinks[link->parentProp].push_back(link);
            }
            else {
                parentLinks[nullptr].push_back(link);
            }
        }
        for (auto& v : parentLinks) {
            if (v.first) {
                v.first->setFlag(PropertyLinkBase::LinkDetached);
                v.first->aboutToSetValue();
            }
            for (auto l : v.second) {
                l->detach();
            }
            if (v.first) {
                v.first->hasSetValue();
                v.first->setFlag(PropertyLinkBase::LinkDetached, false);
            }
        }
        pcDoc = nullptr;
    }

    bool hasXLink(const App::Document* doc) const
    {
        for (auto link : links) {
            auto obj = dynamic_cast<DocumentObject*>(link->getContainer());
            if (obj && obj->getDocument() == doc) {
                return true;
            }
        }
        return false;
    }

    static void breakLinks(App::DocumentObject* obj, bool clear)
    {
        auto doc = obj->getDocument();
        for (auto itD = _DocInfoMap.begin(), itDNext = itD; itD != _DocInfoMap.end();
             itD = itDNext) {
            ++itDNext;
            auto docInfo = itD->second;
            if (docInfo->pcDoc != doc) {
                continue;
            }
            auto& links = docInfo->links;
            std::set<PropertyLinkBase*> parentLinks;
            for (auto it = links.begin(), itNext = it; it != links.end(); it = itNext) {
                ++itNext;
                auto link = *it;
                if (link->_pcLink != obj && !(clear && link->getContainer() == obj)) {
                    continue;
                }
                if (link->parentProp) {
                    parentLinks.insert(link->parentProp);
                }
                else {
                    link->breakLink(obj, clear);
                }
            }
            for (auto link : parentLinks) {
                link->breakLink(obj, clear);
            }
        }
    }
};

void PropertyLinkBase::breakLinks(App::DocumentObject* link,
                                  const std::vector<App::DocumentObject*>& objs,
                                  bool clear)
{
    std::vector<Property*> props;
    for (auto obj : objs) {
        props.clear();
        obj->getPropertyList(props);
        for (auto prop : props) {
            auto linkProp = dynamic_cast<PropertyLinkBase*>(prop);
            if (linkProp) {
                linkProp->breakLink(link, clear);
            }
        }
    }
    DocInfo::breakLinks(link, clear);
}

//**************************************************************************
// PropertyXLink
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE(App::PropertyXLink, App::PropertyLink)

PropertyXLink::PropertyXLink(bool _allowPartial, PropertyLinkBase* parent)
    : parentProp(parent)
{
    setAllowPartial(_allowPartial);
    setAllowExternal(true);
    setSyncSubObject(true);
    if (parent) {
        setContainer(parent->getContainer());
    }
}

PropertyXLink::~PropertyXLink()
{
    try {
        unlink();
    } catch (std::bad_weak_ptr &) {
        FC_WARN("Bad pointer exception caught when destroying PropertyXLink\n");
    }
}

void PropertyXLink::setSyncSubObject(bool enable)
{
    _Flags.set((std::size_t)LinkSyncSubObject, enable);
}

void PropertyXLink::unlink()
{
    if (docInfo) {
        docInfo->remove(this);
        docInfo.reset();
    }
    objectName.clear();
    resetLink();
}

void PropertyXLink::detach()
{
    if (docInfo && _pcLink) {
        aboutToSetValue();
        resetLink();
        updateElementReference(nullptr);
        hasSetValue();
    }
}

void PropertyXLink::aboutToSetValue()
{
    if (parentProp) {
        parentProp->aboutToSetChildValue(*this);
    }
    else {
        PropertyLinkBase::aboutToSetValue();
    }
}

void PropertyXLink::hasSetValue()
{
    if (parentProp) {
        parentProp->hasSetChildValue(*this);
    }
    else {
        PropertyLinkBase::hasSetValue();
    }
}

void PropertyXLink::setSubName(const char* subname)
{
    std::vector<std::string> subs;
    if (!Base::Tools::isNullOrEmpty(subname)) {
        subs.emplace_back(subname);
    }
    aboutToSetValue();
    setSubValues(std::move(subs));
    hasSetValue();
}

void PropertyXLink::setSubValues(std::vector<std::string>&& subs, std::vector<ShadowSub>&& shadows)
{
    _SubList = std::move(subs);
    _ShadowSubList.clear();
    if (shadows.size() == _SubList.size()) {
        _ShadowSubList = std::move(shadows);
        onContainerRestored();  // re-register element references
    }
    else {
        updateElementReference(nullptr);
    }
    checkLabelReferences(_SubList);
}

void PropertyXLink::setValue(App::DocumentObject* lValue)
{
    setValue(lValue, nullptr);
}

void PropertyXLink::setValue(App::DocumentObject* lValue, const char* subname)
{
    std::vector<std::string> subs;
    if (!Base::Tools::isNullOrEmpty(subname)) {
        subs.emplace_back(subname);
    }
    setValue(lValue, std::move(subs));
}

void PropertyXLink::restoreLink(App::DocumentObject* lValue)
{
    assert(!_pcLink && lValue && docInfo);

    auto owner = dynamic_cast<DocumentObject*>(getContainer());
    if (!owner || !owner->isAttachedToDocument()) {
        throw Base::RuntimeError("invalid container");
    }

    bool touched = owner->isTouched();
    setFlag(LinkDetached, false);
    setFlag(LinkRestoring);
    aboutToSetValue();

    if (!owner->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
        lValue->_addBackLink(owner);
    }

    _pcLink = lValue;
    updateElementReference(nullptr);
    hasSetValue();
    setFlag(LinkRestoring, false);

    if (!touched && owner->isTouched() && docInfo && docInfo->pcDoc
        && stamp == docInfo->pcDoc->LastModifiedDate.getValue()) {
        owner->purgeTouched();
    }
}

void PropertyXLink::setValue(App::DocumentObject* lValue,
                             std::vector<std::string>&& subs,
                             std::vector<ShadowSub>&& shadows)
{
    if (_pcLink == lValue && _SubList == subs) {
        return;
    }

    if (lValue && (!lValue->isAttachedToDocument() || !lValue->getDocument())) {
        throw Base::ValueError("Invalid object");
    }

    auto owner = dynamic_cast<DocumentObject*>(getContainer());
    if (!owner || !owner->isAttachedToDocument()) {
        throw Base::RuntimeError("invalid container");
    }

    if (lValue == owner) {
        throw Base::ValueError("self linking");
    }

    aboutToSetValue();

    DocInfoPtr info;
    const char* name = "";
    if (lValue) {
        name = lValue->getNameInDocument();
        if (lValue->getDocument() != owner->getDocument()) {
            if (!docInfo || lValue->getDocument() != docInfo->pcDoc) {
                const char* filename = lValue->getDocument()->getFileName();
                if (!filename || *filename == 0) {
                    throw Base::RuntimeError("Linked document not saved");
                }
                FC_LOG("xlink set to new document " << lValue->getDocument()->getName());
                info = DocInfo::get(filename, owner->getDocument(), this, name);
                assert(info && info->pcDoc == lValue->getDocument());
            }
            else {
                info = docInfo;
            }
        }
    }

    setFlag(LinkDetached, false);

    if (!owner->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
        if (_pcLink) {
            _pcLink->_removeBackLink(owner);
        }
        if (lValue) {
            lValue->_addBackLink(owner);
        }
    }

    if (docInfo != info) {
        unlink();
        docInfo = info;
    }
    if (!docInfo) {
        filePath.clear();
    }
    _pcLink = lValue;
    if (docInfo && docInfo->pcDoc) {
        stamp = docInfo->pcDoc->LastModifiedDate.getValue();
    }
    objectName = name;
    setSubValues(std::move(subs), std::move(shadows));
    hasSetValue();
}

void PropertyXLink::setValue(std::string&& filename,
                             std::string&& name,
                             std::vector<std::string>&& subs,
                             std::vector<ShadowSub>&& shadows)
{
    if (name.empty()) {
        setValue(nullptr, std::move(subs), std::move(shadows));
        return;
    }
    auto owner = dynamic_cast<DocumentObject*>(getContainer());
    if (!owner || !owner->isAttachedToDocument()) {
        throw Base::RuntimeError("invalid container");
    }

    DocumentObject* pObject = nullptr;
    DocInfoPtr info;
    if (!filename.empty()) {
        owner->getDocument()->signalLinkXsetValue(filename);
        info = DocInfo::get(filename.c_str(), owner->getDocument(), this, name.c_str());
        if (info->pcDoc) {
            pObject = info->pcDoc->getObject(name.c_str());
        }
    }
    else {
        pObject = owner->getDocument()->getObject(name.c_str());
    }

    if (pObject) {
        setValue(pObject, std::move(subs), std::move(shadows));
        return;
    }
    setFlag(LinkDetached, false);
    aboutToSetValue();

    if (_pcLink && !owner->testStatus(ObjectStatus::Destroy) && _pcScope != LinkScope::Hidden) {
        _pcLink->_removeBackLink(owner);
    }

    _pcLink = nullptr;
    if (docInfo != info) {
        unlink();
        docInfo = info;
    }
    if (!docInfo) {
        filePath.clear();
    }
    if (docInfo && docInfo->pcDoc) {
        stamp = docInfo->pcDoc->LastModifiedDate.getValue();
    }
    objectName = std::move(name);
    setSubValues(std::move(subs), std::move(shadows));
    hasSetValue();
}

void PropertyXLink::setValue(App::DocumentObject* link,
                             const std::vector<std::string>& subs,
                             std::vector<ShadowSub>&& shadows)
{
    setValue(link, std::vector<std::string>(subs), std::move(shadows));
}

App::Document* PropertyXLink::getDocument() const
{
    return docInfo ? docInfo->pcDoc : nullptr;
}

const char* PropertyXLink::getDocumentPath() const
{
    return docInfo ? docInfo->filePath() : filePath.c_str();
}

const char* PropertyXLink::getObjectName() const
{
    return objectName.c_str();
}

bool PropertyXLink::upgrade(Base::XMLReader& reader, const char* typeName)
{
    if (strcmp(typeName, App::PropertyLinkGlobal::getClassTypeId().getName()) == 0
        || strcmp(typeName, App::PropertyLink::getClassTypeId().getName()) == 0
        || strcmp(typeName, App::PropertyLinkChild::getClassTypeId().getName()) == 0) {
        PropertyLink::Restore(reader);
        return true;
    }
    FC_ERR("Cannot upgrade from " << typeName);
    return false;
}

int PropertyXLink::checkRestore(std::string* msg) const
{
    if (!docInfo) {
        if (!_pcLink && !objectName.empty()) {
            // this condition means linked object not found
            if (msg) {
                std::ostringstream ss;
                ss << "Link not restored" << std::endl;
                ss << "Object: " << objectName;
                if (!filePath.empty()) {
                    ss << std::endl << "File: " << filePath;
                }
                *msg = ss.str();
            }
            return 2;
        }
        return 0;
    }
    if (!_pcLink) {
        if (testFlag(LinkSilentRestore)) {
            return 0;
        }
        if (testFlag(LinkAllowPartial)
            && (!docInfo->pcDoc || docInfo->pcDoc->testStatus(App::Document::PartialDoc))) {
            return 0;
        }
        if (msg) {
            std::ostringstream ss;
            ss << "Link not restored" << std::endl;
            ss << "Linked object: " << objectName;
            if (docInfo->pcDoc) {
                ss << std::endl << "Linked document: " << docInfo->pcDoc->Label.getValue();
            }
            else if (!filePath.empty()) {
                ss << std::endl << "Linked file: " << filePath;
            }
            *msg = ss.str();
        }
        return 2;
    }
    if (!docInfo->pcDoc || stamp == docInfo->pcDoc->LastModifiedDate.getValue()) {
        return 0;
    }

    if (msg) {
        std::ostringstream ss;
        ss << "Time stamp changed on link " << _pcLink->getFullName();
        *msg = ss.str();
    }
    return 1;
}

void PropertyXLink::afterRestore()
{
    assert(_SubList.size() == _ShadowSubList.size());
    if (!testFlag(LinkRestoreLabel) || !_pcLink || !_pcLink->isAttachedToDocument()) {
        return;
    }
    setFlag(LinkRestoreLabel, false);
    for (size_t i = 0; i < _SubList.size(); ++i) {
        restoreLabelReference(_pcLink, _SubList[i], &_ShadowSubList[i]);
    }
}

void PropertyXLink::onContainerRestored()
{
    if (!_pcLink || !_pcLink->isAttachedToDocument()) {
        return;
    }
    for (size_t i = 0; i < _SubList.size(); ++i) {
        _registerElementReference(_pcLink, _SubList[i], _ShadowSubList[i]);
    }
}

void PropertyXLink::updateElementReference(DocumentObject* feature, bool reverse, bool notify)
{
    if (!updateLinkReference(this,
                             feature,
                             reverse,
                             notify,
                             _pcLink,
                             _SubList,
                             _mapped,
                             _ShadowSubList)) {
        return;
    }
    if (notify) {
        hasSetValue();
    }
}

bool PropertyXLink::referenceChanged() const
{
    return !_mapped.empty();
}

void PropertyXLink::Save(Base::Writer& writer) const
{
    auto owner = dynamic_cast<const DocumentObject*>(getContainer());
    if (!owner || !owner->getDocument()) {
        return;
    }

    assert(_SubList.size() == _ShadowSubList.size());

    auto exporting = owner->isExporting();
    if (_pcLink && exporting && _pcLink->isExporting()) {
        // this means, we are exporting the owner and the linked object together.
        // Lets save the export name
        writer.Stream() << writer.ind() << "<XLink name=\"" << _pcLink->getExportName();
    }
    else {
        const char* path = filePath.c_str();
        std::string _path;
        if (exporting) {
            // Here means we are exporting the owner but not exporting the
            // linked object.  Try to use absolute file path for easy transition
            // into document at different directory
            if (docInfo) {
                _path = docInfo->filePath();
            }
            else {
                auto pDoc = owner->getDocument();
                const char* docPath = pDoc->getFileName();
                if (!Base::Tools::isNullOrEmpty(docPath)) {
                    if (!filePath.empty()) {
                        _path = DocInfo::getDocPath(filePath.c_str(), pDoc, false);
                    }
                    else {
                        _path = docPath;
                    }
                }
                else {
                    FC_WARN("PropertyXLink export without saving the document");
                }
            }
            if (!_path.empty()) {
                path = _path.c_str();
            }
        }
        writer.Stream() << writer.ind() << "<XLink file=\"" << encodeAttribute(path)
                        << "\" stamp=\""
                        << (docInfo && docInfo->pcDoc ? docInfo->pcDoc->LastModifiedDate.getValue()
                                                      : "")
                        << "\" name=\"" << objectName;
    }

    if (testFlag(LinkAllowPartial)) {
        writer.Stream() << "\" partial=\"1";
    }

    if (_SubList.empty()) {
        writer.Stream() << "\"/>" << std::endl;
    }
    else if (_SubList.size() == 1) {
        const auto& subName = _SubList[0];
        const auto& shadowSub = _ShadowSubList[0];
        const auto& sub = shadowSub.oldName.empty() ? subName : shadowSub.oldName;
        if (exporting) {
            std::string exportName;
            writer.Stream() << "\" sub=\""
                            << encodeAttribute(exportSubName(exportName, _pcLink, sub.c_str()));
            if (!shadowSub.oldName.empty() && shadowSub.newName == subName) {
                writer.Stream() << "\" " ATTR_MAPPED "=\"1";
            }
        }
        else {
            writer.Stream() << "\" sub=\"" << encodeAttribute(sub);
            if (!sub.empty()) {
                if (sub != subName) {
                    writer.Stream() << "\" " ATTR_SHADOWED "=\"" << encodeAttribute(subName);
                }
                else if (!shadowSub.newName.empty()) {
                    writer.Stream()
                        << "\" " ATTR_SHADOW "=\"" << encodeAttribute(shadowSub.newName);
                }
            }
        }
        writer.Stream() << "\"/>" << std::endl;
    }
    else {
        writer.Stream() << "\" count=\"" << _SubList.size() << "\">" << std::endl;
        writer.incInd();
        for (unsigned int i = 0; i < _SubList.size(); i++) {
            const auto& shadow = _ShadowSubList[i];
            // shadow.oldName stores the old style element name. For backward
            // compatibility reason, we shall store the old name into attribute
            // 'value' whenever possible.
            const auto& sub = shadow.oldName.empty() ? _SubList[i] : shadow.oldName;
            writer.Stream() << writer.ind() << "<Sub value=\"";
            if (exporting) {
                std::string exportName;
                writer.Stream() << encodeAttribute(exportSubName(exportName, _pcLink, sub.c_str()));
                if (!shadow.oldName.empty() && shadow.newName == _SubList[i]) {
                    writer.Stream() << "\" " ATTR_MAPPED "=\"1";
                }
            }
            else {
                writer.Stream() << encodeAttribute(sub);
                if (!_SubList[i].empty()) {
                    if (sub != _SubList[i]) {
                        writer.Stream()
                            << "\" " ATTR_SHADOWED "=\"" << encodeAttribute(_SubList[i]);
                    }
                    else if (!shadow.newName.empty()) {
                        writer.Stream()
                            << "\" " ATTR_SHADOW "=\"" << encodeAttribute(shadow.newName);
                    }
                }
            }
            writer.Stream() << "\"/>" << endl;
        }
        writer.decInd();
        writer.Stream() << writer.ind() << "</XLink>" << endl;
    }
}

void PropertyXLink::Restore(Base::XMLReader& reader)
{
    // read my element
    reader.readElement("XLink");
    std::string stampAttr, file;
    if (reader.hasAttribute("stamp")) {
        stampAttr = reader.getAttribute("stamp");
    }
    if (reader.hasAttribute("file")) {
        file = reader.getAttribute("file");
    }

    setFlag(LinkAllowPartial,
            reader.hasAttribute("partial") && reader.getAttributeAsInteger("partial"));
    std::string name;
    if (file.empty()) {
        name = reader.getName(reader.getAttribute("name"));
    }
    else {
        name = reader.getAttribute("name");
    }

    assert(getContainer()->isDerivedFrom<App::DocumentObject>());
    DocumentObject* object = nullptr;
    if (!name.empty() && file.empty()) {
        DocumentObject* parent = static_cast<DocumentObject*>(getContainer());
        Document* document = parent->getDocument();
        object = document ? document->getObject(name.c_str()) : nullptr;
        if (!object) {
            if (reader.isVerbose()) {
                FC_WARN("Lost link to '" << name
                                         << "' while loading, maybe "
                                            "an object was not loaded correctly");
            }
        }
    }

    std::vector<std::string> subs;
    std::vector<ShadowSub> shadows;
    std::vector<int> mapped;
    bool restoreLabel = false;
    if (reader.hasAttribute("sub")) {
        if (reader.hasAttribute(ATTR_MAPPED)) {
            mapped.push_back(0);
        }
        subs.emplace_back();
        auto& subname = subs.back();
        shadows.emplace_back();
        auto& shadow = shadows.back();
        shadow.oldName = importSubName(reader, reader.getAttribute("sub"), restoreLabel);
        if (reader.hasAttribute(ATTR_SHADOWED) && !IGNORE_SHADOW) {
            subname = shadow.newName =
                importSubName(reader, reader.getAttribute(ATTR_SHADOWED), restoreLabel);
        }
        else {
            subname = shadow.oldName;
            if (reader.hasAttribute(ATTR_SHADOW) && !IGNORE_SHADOW) {
                shadow.newName =
                    importSubName(reader, reader.getAttribute(ATTR_SHADOW), restoreLabel);
            }
        }
    }
    else if (reader.hasAttribute("count")) {
        int count = reader.getAttributeAsInteger("count");
        subs.resize(count);
        shadows.resize(count);
        for (int i = 0; i < count; i++) {
            reader.readElement("Sub");
            shadows[i].oldName = importSubName(reader, reader.getAttribute("value"), restoreLabel);
            if (reader.hasAttribute(ATTR_SHADOWED) && !IGNORE_SHADOW) {
                subs[i] = shadows[i].newName =
                    importSubName(reader, reader.getAttribute(ATTR_SHADOWED), restoreLabel);
            }
            else {
                subs[i] = shadows[i].oldName;
                if (reader.hasAttribute(ATTR_SHADOW) && !IGNORE_SHADOW) {
                    shadows[i].newName =
                        importSubName(reader, reader.getAttribute(ATTR_SHADOW), restoreLabel);
                }
            }
            if (reader.hasAttribute(ATTR_MAPPED)) {
                mapped.push_back(i);
            }
        }
        reader.readEndElement("XLink");
    }
    setFlag(LinkRestoreLabel, restoreLabel);

    if (name.empty()) {
        setValue(nullptr);
        return;
    }

    if (!file.empty() || (!object && !name.empty())) {
        this->stamp = stampAttr;
        setValue(std::move(file), std::move(name), std::move(subs), std::move(shadows));
    }
    else {
        setValue(object, std::move(subs), std::move(shadows));
    }
    _mapped = std::move(mapped);
}

Property*
PropertyXLink::CopyOnImportExternal(const std::map<std::string, std::string>& nameMap) const
{
    auto owner = Base::freecad_dynamic_cast<const DocumentObject>(getContainer());
    if (!owner || !owner->getDocument() || !_pcLink || !_pcLink->isAttachedToDocument()) {
        return nullptr;
    }

    auto subs = updateLinkSubs(_pcLink, _SubList, &tryImportSubName, owner->getDocument(), nameMap);
    auto linked = tryImport(owner->getDocument(), _pcLink, nameMap);
    if (subs.empty() && linked == _pcLink) {
        return nullptr;
    }

    std::unique_ptr<PropertyXLink> p(new PropertyXLink);
    copyTo(*p, linked, &subs);
    return p.release();
}

Property* PropertyXLink::CopyOnLinkReplace(const App::DocumentObject* parent,
                                           App::DocumentObject* oldObj,
                                           App::DocumentObject* newObj) const
{
    auto res = tryReplaceLinkSubs(getContainer(), _pcLink, parent, oldObj, newObj, _SubList);
    if (!res.first) {
        return nullptr;
    }
    std::unique_ptr<PropertyXLink> p(new PropertyXLink);
    copyTo(*p, res.first, &res.second);
    return p.release();
}

Property* PropertyXLink::CopyOnLabelChange(App::DocumentObject* obj,
                                           const std::string& ref,
                                           const char* newLabel) const
{
    auto owner = dynamic_cast<const DocumentObject*>(getContainer());
    if (!owner || !owner->getDocument() || !_pcLink || !_pcLink->isAttachedToDocument()) {
        return nullptr;
    }
    auto subs = updateLinkSubs(_pcLink, _SubList, &updateLabelReference, obj, ref, newLabel);
    if (subs.empty()) {
        return nullptr;
    }
    std::unique_ptr<PropertyXLink> p(new PropertyXLink);
    copyTo(*p, _pcLink, &subs);
    return p.release();
}

void PropertyXLink::copyTo(PropertyXLink& other,
                           DocumentObject* linked,
                           std::vector<std::string>* subs) const
{
    if (!linked) {
        linked = _pcLink;
    }
    if (linked && linked->isAttachedToDocument()) {
        other.docName = linked->getDocument()->getName();
        other.objectName = linked->getNameInDocument();
        other.docInfo.reset();
        other.filePath.clear();
    }
    else {
        other.objectName = objectName;
        other.docName.clear();
        other.docInfo = docInfo;
        other.filePath = filePath;
    }
    if (subs) {
        other._SubList = std::move(*subs);
    }
    else {
        other._SubList = _SubList;
        other._ShadowSubList = _ShadowSubList;
    }
    other._Flags = _Flags;
}

Property* PropertyXLink::Copy() const
{
    std::unique_ptr<PropertyXLink> p(new PropertyXLink);
    copyTo(*p);
    return p.release();
}

void PropertyXLink::Paste(const Property& from)
{
    if (!from.isDerivedFrom<PropertyXLink>()) {
        throw Base::TypeError("Incompatible property to paste to");
    }

    const auto& other = static_cast<const PropertyXLink&>(from);
    if (!other.docName.empty()) {
        auto doc = GetApplication().getDocument(other.docName.c_str());
        if (!doc) {
            FC_WARN("Document '" << other.docName << "' not found");
            return;
        }
        auto obj = doc->getObject(other.objectName.c_str());
        if (!obj) {
            FC_WARN("Object '" << other.docName << '#' << other.objectName << "' not found");
            return;
        }
        setValue(obj,
                 std::vector<std::string>(other._SubList),
                 std::vector<ShadowSub>(other._ShadowSubList));
    }
    else {
        setValue(std::string(other.filePath),
                 std::string(other.objectName),
                 std::vector<std::string>(other._SubList),
                 std::vector<ShadowSub>(other._ShadowSubList));
    }
    setFlag(LinkAllowPartial, other.testFlag(LinkAllowPartial));
}

bool PropertyXLink::supportXLink(const App::Property* prop)
{
    return prop->isDerivedFrom<PropertyXLink>()
        || prop->isDerivedFrom<PropertyXLinkSubList>()
        || prop->isDerivedFrom<PropertyXLinkContainer>();
}

bool PropertyXLink::hasXLink(const App::Document* doc)
{
    for (auto& v : _DocInfoMap) {
        if (v.second->hasXLink(doc)) {
            return true;
        }
    }
    return false;
}

bool PropertyXLink::hasXLink(const std::vector<App::DocumentObject*>& objs,
                             std::vector<App::Document*>* unsaved)
{
    std::set<App::Document*> docs;
    bool ret = false;
    for (auto o : objs) {
        if (o && o->isAttachedToDocument() && docs.insert(o->getDocument()).second) {
            if (!hasXLink(o->getDocument())) {
                continue;
            }
            if (!unsaved) {
                return true;
            }
            ret = true;
            if (!o->getDocument()->isSaved()) {
                unsaved->push_back(o->getDocument());
            }
        }
    }
    return ret;
}

void PropertyXLink::restoreDocument(const App::Document& doc)
{
    DocInfo::restoreDocument(doc);
}

std::map<App::Document*, std::set<App::Document*>>
PropertyXLink::getDocumentOutList(App::Document* doc)
{
    std::map<App::Document*, std::set<App::Document*>> ret;
    for (auto& v : _DocInfoMap) {
        for (auto link : v.second->links) {
            if (!v.second->pcDoc || link->getScope() == LinkScope::Hidden
                || link->testStatus(Property::PropTransient)
                || link->testStatus(Property::Transient)
                || link->testStatus(Property::PropNoPersist)) {
                continue;
            }
            auto obj = dynamic_cast<App::DocumentObject*>(link->getContainer());
            if (!obj || !obj->isAttachedToDocument() || !obj->getDocument()) {
                continue;
            }
            if (doc && obj->getDocument() != doc) {
                continue;
            }
            ret[obj->getDocument()].insert(v.second->pcDoc);
        }
    }
    return ret;
}

std::map<App::Document*, std::set<App::Document*>>
PropertyXLink::getDocumentInList(App::Document* doc)
{
    std::map<App::Document*, std::set<App::Document*>> ret;
    for (auto& v : _DocInfoMap) {
        if (!v.second->pcDoc || (doc && doc != v.second->pcDoc)) {
            continue;
        }
        auto& docs = ret[v.second->pcDoc];
        for (auto link : v.second->links) {
            if (link->getScope() == LinkScope::Hidden || link->testStatus(Property::PropTransient)
                || link->testStatus(Property::Transient)
                || link->testStatus(Property::PropNoPersist)) {
                continue;
            }
            auto obj = dynamic_cast<App::DocumentObject*>(link->getContainer());
            if (obj && obj->isAttachedToDocument() && obj->getDocument()) {
                docs.insert(obj->getDocument());
            }
        }
    }
    return ret;
}

PyObject* PropertyXLink::getPyObject()
{
    if (!_pcLink) {
        Py_Return;
    }
    const auto& subs = getSubValues(false);
    if (subs.empty()) {
        return _pcLink->getPyObject();
    }
    Py::Tuple ret(2);
    ret.setItem(0, Py::Object(_pcLink->getPyObject(), true));
    PropertyString propString;
    if (subs.size() == 1) {
        propString.setValue(subs.front());
        ret.setItem(1, Py::asObject(propString.getPyObject()));
    }
    else {
        Py::List list(subs.size());
        int i = 0;
        for (auto& sub : subs) {
            propString.setValue(sub);
            list[i++] = Py::asObject(propString.getPyObject());
        }
        ret.setItem(1, list);
    }
    return Py::new_reference_to(ret);
}

void PropertyXLink::setPyObject(PyObject* value)
{
    if (PySequence_Check(value)) {
        Py::Sequence seq(value);
        if (seq.size() != 2) {
            throw Base::ValueError("Expect input sequence of size 2");
        }
        std::vector<std::string> subs;
        Py::Object pyObj(seq[0].ptr());
        Py::Object pySub(seq[1].ptr());
        if (pyObj.isNone()) {
            setValue(nullptr);
            return;
        }
        else if (!PyObject_TypeCheck(pyObj.ptr(), &DocumentObjectPy::Type)) {
            throw Base::TypeError("Expect the first element to be of 'DocumentObject'");
        }
        PropertyString propString;
        if (pySub.isString()) {
            propString.setPyObject(pySub.ptr());
            subs.push_back(propString.getStrValue());
        }
        else if (pySub.isSequence()) {
            Py::Sequence seq(pySub);
            subs.reserve(seq.size());
            for (Py_ssize_t i = 0; i < seq.size(); ++i) {
                Py::Object sub(seq[i]);
                if (!sub.isString()) {
                    throw Base::TypeError("Expect only string inside second argument");
                }
                propString.setPyObject(sub.ptr());
                subs.push_back(propString.getStrValue());
            }
        }
        else {
            throw Base::TypeError("Expect the second element to be a string or sequence of string");
        }
        setValue(static_cast<DocumentObjectPy*>(pyObj.ptr())->getDocumentObjectPtr(),
                 std::move(subs));
    }
    else if (PyObject_TypeCheck(value, &(DocumentObjectPy::Type))) {
        setValue(static_cast<DocumentObjectPy*>(value)->getDocumentObjectPtr());
    }
    else if (Py_None == value) {
        setValue(nullptr);
    }
    else {
        throw Base::TypeError(
            "type must be 'DocumentObject', 'None', or '(DocumentObject, SubName)' or "
            "'DocumentObject, [SubName..])");
    }
}

const char* PropertyXLink::getSubName(bool newStyle) const
{
    if (_SubList.empty() || _ShadowSubList.empty()) {
        return "";
    }
    return getSubNameWithStyle(_SubList[0], _ShadowSubList[0], newStyle, tmpShadow).c_str();
}

void PropertyXLink::getLinks(std::vector<App::DocumentObject*>& objs,
                             bool all,
                             std::vector<std::string>* subs,
                             bool newStyle) const
{
    if ((all || _pcScope != LinkScope::Hidden) && _pcLink && _pcLink->isAttachedToDocument()) {
        objs.push_back(_pcLink);
        if (subs && _SubList.size() == _ShadowSubList.size()) {
            *subs = getSubValues(newStyle);
        }
    }
}

void PropertyXLink::getLinksTo(std::vector<App::ObjectIdentifier>& identifiers,
                               App::DocumentObject* obj,
                               const char* subname,
                               bool all) const
{
    if (all || _pcScope != LinkScope::Hidden) {
        if (obj && obj == _pcLink) {
            _getLinksTo(identifiers, obj, subname, _SubList, _ShadowSubList);
        }
    }
}

bool PropertyXLink::adjustLink(const std::set<App::DocumentObject*>& inList)
{
    if (_pcScope == LinkScope::Hidden) {
        return false;
    }
    if (!_pcLink || !_pcLink->isAttachedToDocument() || !inList.count(_pcLink)) {
        return false;
    }
    auto subs = _SubList;
    auto link = adjustLinkSubs(this, inList, _pcLink, subs);
    if (link) {
        setValue(link, std::move(subs));
        return true;
    }
    return false;
}

std::vector<std::string> PropertyXLink::getSubValues(bool newStyle) const
{
    assert(_SubList.size() == _ShadowSubList.size());
    std::vector<std::string> ret;
    ret.reserve(_SubList.size());
    std::string tmp;
    for (size_t i = 0; i < _ShadowSubList.size(); ++i) {
        ret.push_back(getSubNameWithStyle(_SubList[i], _ShadowSubList[i], newStyle, tmp));
    }
    return ret;
}

std::vector<std::string> PropertyXLink::getSubValuesStartsWith(const char* starter,
                                                               bool newStyle) const
{
    (void)newStyle;

    std::vector<std::string> temp;
    for (const auto& it : _SubList) {
        if (strncmp(starter, it.c_str(), strlen(starter)) == 0) {
            temp.push_back(it);
        }
    }
    return temp;
}

void PropertyXLink::setAllowPartial(bool enable)
{
    setFlag(LinkAllowPartial, enable);
    if (enable) {
        return;
    }
    auto owner = dynamic_cast<const DocumentObject*>(getContainer());
    if (!owner) {
        return;
    }
    if (!App::GetApplication().isRestoring() && !owner->getDocument()->isPerformingTransaction()
        && !_pcLink && docInfo && !filePath.empty() && !objectName.empty()
        && (!docInfo->pcDoc || docInfo->pcDoc->testStatus(Document::PartialDoc))) {
        auto path = docInfo->getDocPath(filePath.c_str(), owner->getDocument(), false);
        if (!path.empty()) {
            App::GetApplication().openDocument(path.c_str());
        }
    }
}

//**************************************************************************
// PropertyXLinkSub
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE(App::PropertyXLinkSub, App::PropertyXLink)
TYPESYSTEM_SOURCE(App::PropertyXLinkSubHidden, App::PropertyXLinkSub)

PropertyXLinkSub::PropertyXLinkSub(bool allowPartial, PropertyLinkBase* parent)
    : PropertyXLink(allowPartial, parent)
{}

PropertyXLinkSub::~PropertyXLinkSub() = default;

bool PropertyXLinkSub::upgrade(Base::XMLReader& reader, const char* typeName)
{
    if (strcmp(typeName, PropertyLinkSubGlobal::getClassTypeId().getName()) == 0
        || strcmp(typeName, PropertyLinkSub::getClassTypeId().getName()) == 0
        || strcmp(typeName, PropertyLinkSubChild::getClassTypeId().getName()) == 0) {
        App::PropertyLinkSub linkProp;
        linkProp.setContainer(getContainer());
        linkProp.Restore(reader);
        setValue(linkProp.getValue(), linkProp.getSubValues());
        return true;
    }
    return PropertyXLink::upgrade(reader, typeName);
}

PyObject* PropertyXLinkSub::getPyObject()
{
    if (!_pcLink) {
        Py_Return;
    }
    Py::Tuple ret(2);
    ret.setItem(0, Py::Object(_pcLink->getPyObject(), true));
    const auto& subs = getSubValues(false);
    Py::List list(subs.size());
    int i = 0;
    PropertyString propString;
    for (auto& sub : subs) {
        propString.setValue(sub);
        list[i++] = Py::asObject(propString.getPyObject());
    }
    ret.setItem(1, list);
    return Py::new_reference_to(ret);
}

//**************************************************************************
// PropertyXLinkSubList
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE(App::PropertyXLinkSubList, App::PropertyLinkBase)

//**************************************************************************
// Construction/Destruction


PropertyXLinkSubList::PropertyXLinkSubList()
{
    _pcScope = LinkScope::Global;
    setSyncSubObject(true);
}

PropertyXLinkSubList::~PropertyXLinkSubList() = default;

void PropertyXLinkSubList::setSyncSubObject(bool enable)
{
    _Flags.set((std::size_t)LinkSyncSubObject, enable);
}

int PropertyXLinkSubList::getSize() const
{
    return static_cast<int>(_Links.size());
}

void PropertyXLinkSubList::setValue(DocumentObject* lValue, const char* SubName)
{
    std::map<DocumentObject*, std::vector<std::string>> values;
    if (lValue) {
        auto& subs = values[lValue];
        if (SubName) {
            subs.emplace_back(SubName);
        }
    }
    setValues(std::move(values));
}

void PropertyXLinkSubList::setValues(const std::vector<DocumentObject*>& lValue,
                                     const std::vector<const char*>& lSubNames)
{
#define CHECK_SUB_SIZE(_l, _r)                                                                     \
    do {                                                                                           \
        if (_l.size() != _r.size())                                                                \
            FC_THROWM(Base::ValueError, "object and subname size mismatch");                       \
    } while (0)
    CHECK_SUB_SIZE(lValue, lSubNames);
    std::map<DocumentObject*, std::vector<std::string>> values;
    int i = 0;
    for (auto& obj : lValue) {
        const char* sub = lSubNames[i++];
        if (sub) {
            values[obj].emplace_back(sub);
        }
    }
    setValues(std::move(values));
}

void PropertyXLinkSubList::setValues(const std::vector<DocumentObject*>& lValue,
                                     const std::vector<std::string>& lSubNames)
{
    CHECK_SUB_SIZE(lValue, lSubNames);
    std::map<DocumentObject*, std::vector<std::string>> values;
    int i = 0;
    for (auto& obj : lValue) {
        values[obj].push_back(lSubNames[i++]);
    }
    setValues(std::move(values));
}

void PropertyXLinkSubList::setSubListValues(const std::vector<PropertyLinkSubList::SubSet>& svalues)
{
    std::map<DocumentObject*, std::vector<std::string>> values;
    for (auto& v : svalues) {
        auto& s = values[v.first];
        s.reserve(s.size() + v.second.size());
        s.insert(s.end(), v.second.begin(), v.second.end());
    }
    setValues(std::move(values));
}

void PropertyXLinkSubList::setValues(
    const std::map<App::DocumentObject*, std::vector<std::string>>& values)
{
    setValues(std::map<App::DocumentObject*, std::vector<std::string>>(values));
}

void PropertyXLinkSubList::setValues(
    std::map<App::DocumentObject*, std::vector<std::string>>&& values)
{
    for (auto& v : values) {
        if (!v.first || !v.first->isAttachedToDocument()) {
            FC_THROWM(Base::ValueError, "invalid document object");
        }
    }

    atomic_change guard(*this);

    for (auto it = _Links.begin(), itNext = it; it != _Links.end(); it = itNext) {
        ++itNext;
        auto iter = values.find(it->getValue());
        if (iter == values.end()) {
            _Links.erase(it);
            continue;
        }
        it->setSubValues(std::move(iter->second));
        values.erase(iter);
    }

    for (auto& v : values) {
        _Links.emplace_back(testFlag(LinkAllowPartial), this);
        _Links.back().setValue(v.first, std::move(v.second));
    }
    guard.tryInvoke();
}

void PropertyXLinkSubList::addValue(App::DocumentObject* obj,
                                    const std::vector<std::string>& subs,
                                    bool reset)
{
    addValue(obj, std::vector<std::string>(subs), reset);
}

void PropertyXLinkSubList::addValue(App::DocumentObject* obj,
                                    std::vector<std::string>&& subs,
                                    bool reset)
{

    if (!obj || !obj->isAttachedToDocument()) {
        FC_THROWM(Base::ValueError, "invalid document object");
    }

    for (auto& l : _Links) {
        if (l.getValue() == obj) {
            auto s = l.getSubValues();
            if (s.empty() || reset) {
                l.setSubValues(std::move(subs));
            }
            else {
                s.reserve(s.size() + subs.size());
                std::move(subs.begin(), subs.end(), std::back_inserter(s));
                l.setSubValues(std::move(s));
            }
            return;
        }
    }
    atomic_change guard(*this);
    _Links.emplace_back(testFlag(LinkAllowPartial), this);
    _Links.back().setValue(obj, std::move(subs));
    guard.tryInvoke();
}

void PropertyXLinkSubList::setValue(DocumentObject* lValue, const std::vector<std::string>& SubList)
{
    std::map<DocumentObject*, std::vector<std::string>> values;
    if (lValue) {
        values[lValue] = SubList;
    }
    setValues(std::move(values));
}

void PropertyXLinkSubList::setValues(const std::vector<DocumentObject*>& values)
{
    atomic_change guard(*this);
    _Links.clear();
    for (auto obj : values) {
        _Links.emplace_back(testFlag(LinkAllowPartial), this);
        _Links.back().setValue(obj);
    }
    guard.tryInvoke();
}

void PropertyXLinkSubList::set1Value(int idx,
                                     DocumentObject* value,
                                     const std::vector<std::string>& SubList)
{
    if (idx < -1 || idx > getSize()) {
        throw Base::RuntimeError("index out of bound");
    }

    if (idx < 0 || idx + 1 == getSize()) {
        if (SubList.empty()) {
            addValue(value, SubList);
            return;
        }
        atomic_change guard(*this);
        _Links.emplace_back(testFlag(LinkAllowPartial), this);
        _Links.back().setValue(value);
        guard.tryInvoke();
        return;
    }

    auto it = _Links.begin();
    for (; idx; --idx) {
        ++it;
    }
    it->setValue(value, SubList);
}

const string PropertyXLinkSubList::getPyReprString() const
{
    if (_Links.empty()) {
        return std::string("None");
    }
    std::ostringstream ss;
    ss << '[';
    for (auto& link : _Links) {
        auto obj = link.getValue();
        if (!obj || !obj->isAttachedToDocument()) {
            continue;
        }
        ss << "(App.getDocument('" << obj->getDocument()->getName() << "').getObject('"
           << obj->getNameInDocument() << "'),  (";
        const auto& subs = link.getSubValues();
        if (subs.empty()) {
            ss << "''";
        }
        else {
            for (auto& sub : subs) {
                ss << "'" << sub << "',";
            }
        }
        ss << ")), ";
    }
    ss << ']';
    return ss.str();
}

DocumentObject* PropertyXLinkSubList::getValue() const
{
    if (!_Links.empty()) {
        return _Links.begin()->getValue();
    }
    return nullptr;
}

int PropertyXLinkSubList::removeValue(App::DocumentObject* lValue)
{
    atomic_change guard(*this, false);
    int ret = 0;
    for (auto it = _Links.begin(); it != _Links.end();) {
        if (it->getValue() != lValue) {
            ++it;
        }
        else {
            guard.aboutToChange();
            it = _Links.erase(it);
            ++ret;
        }
    }
    guard.tryInvoke();
    return ret;
}

PyObject* PropertyXLinkSubList::getPyObject()
{
    Py::List list;
    for (auto& link : _Links) {
        auto obj = link.getValue();
        if (!obj || !obj->isAttachedToDocument()) {
            continue;
        }

        Py::Tuple tup(2);
        tup[0] = Py::asObject(obj->getPyObject());

        const auto& subs = link.getSubValues();
        Py::Tuple items(subs.size());
        for (std::size_t j = 0; j < subs.size(); j++) {
            items[j] = Py::String(subs[j]);
        }
        tup[1] = items;
        list.append(tup);
    }
    return Py::new_reference_to(list);
}

void PropertyXLinkSubList::setPyObject(PyObject* value)
{
    try {  // try PropertyLinkSub syntax
        PropertyLinkSub dummy;
        dummy.setAllowExternal(true);
        dummy.setPyObject(value);
        this->setValue(dummy.getValue(), dummy.getSubValues());
        return;
    }
    catch (Base::Exception&) {
    }

    if (!PyTuple_Check(value) && !PyList_Check(value)) {
        throw Base::TypeError(
            "Invalid type. Accepts (DocumentObject, (subname...)) or sequence of such type.");
    }
    Py::Sequence seq(value);
    std::map<DocumentObject*, std::vector<std::string>> values;
    try {
        for (Py_ssize_t i = 0; i < seq.size(); ++i) {
            PropertyLinkSub link;
            link.setAllowExternal(true);
            link.setPyObject(seq[i].ptr());
            const auto& subs = link.getSubValues();
            auto& s = values[link.getValue()];
            s.reserve(s.size() + subs.size());
            s.insert(s.end(), subs.begin(), subs.end());
        }
    }
    catch (Base::Exception&) {
        throw Base::TypeError(
            "Invalid type inside sequence. Must be type of (DocumentObject, (subname...))");
    }
    setValues(std::move(values));
}

void PropertyXLinkSubList::afterRestore()
{
    for (auto& l : _Links) {
        l.afterRestore();
    }
}

void PropertyXLinkSubList::onContainerRestored()
{
    for (auto& l : _Links) {
        l.onContainerRestored();
    }
}

void PropertyXLinkSubList::updateElementReference(DocumentObject* feature,
                                                  bool reverse,
                                                  bool notify)
{
    for (auto& l : _Links) {
        l.updateElementReference(feature, reverse, notify);
    }
}

bool PropertyXLinkSubList::referenceChanged() const
{
    for (auto& l : _Links) {
        if (l.referenceChanged()) {
            return true;
        }
    }
    return false;
}

void PropertyXLinkSubList::Save(Base::Writer& writer) const
{
    writer.Stream() << writer.ind() << "<XLinkSubList count=\"" << _Links.size();
    if (testFlag(LinkAllowPartial)) {
        writer.Stream() << "\" partial=\"1";
    }
    writer.Stream() << "\">" << endl;
    writer.incInd();
    for (auto& l : _Links) {
        l.Save(writer);
    }
    writer.decInd();
    writer.Stream() << writer.ind() << "</XLinkSubList>" << endl;
}

void PropertyXLinkSubList::Restore(Base::XMLReader& reader)
{
    reader.readElement("XLinkSubList");
    setFlag(LinkAllowPartial,
            reader.hasAttribute("partial") && reader.getAttributeAsInteger("partial"));
    int count = reader.getAttributeAsInteger("count");
    atomic_change guard(*this, false);
    _Links.clear();
    for (int i = 0; i < count; ++i) {
        _Links.emplace_back(false, this);
        _Links.back().Restore(reader);
    }
    reader.readEndElement("XLinkSubList");
    guard.tryInvoke();
}

Property*
PropertyXLinkSubList::CopyOnImportExternal(const std::map<std::string, std::string>& nameMap) const
{
    std::unique_ptr<Property> copy;
    auto it = _Links.begin();
    for (; it != _Links.end(); ++it) {
        copy.reset(it->CopyOnImportExternal(nameMap));
        if (copy) {
            break;
        }
    }
    if (!copy) {
        return nullptr;
    }
    std::unique_ptr<PropertyXLinkSubList> p(new PropertyXLinkSubList);
    for (auto iter = _Links.begin(); iter != it; ++iter) {
        p->_Links.emplace_back();
        iter->copyTo(p->_Links.back());
    }
    p->_Links.emplace_back();
    static_cast<PropertyXLinkSub&>(*copy).copyTo(p->_Links.back());
    for (++it; it != _Links.end(); ++it) {
        p->_Links.emplace_back();
        copy.reset(it->CopyOnImportExternal(nameMap));
        if (copy) {
            static_cast<PropertyXLinkSub&>(*copy).copyTo(p->_Links.back());
        }
        else {
            it->copyTo(p->_Links.back());
        }
    }
    return p.release();
}

Property* PropertyXLinkSubList::CopyOnLabelChange(App::DocumentObject* obj,
                                                  const std::string& ref,
                                                  const char* newLabel) const
{
    std::unique_ptr<Property> copy;
    auto it = _Links.begin();
    for (; it != _Links.end(); ++it) {
        copy.reset(it->CopyOnLabelChange(obj, ref, newLabel));
        if (copy) {
            break;
        }
    }
    if (!copy) {
        return nullptr;
    }
    std::unique_ptr<PropertyXLinkSubList> p(new PropertyXLinkSubList);
    for (auto iter = _Links.begin(); iter != it; ++iter) {
        p->_Links.emplace_back();
        iter->copyTo(p->_Links.back());
    }
    p->_Links.emplace_back();
    static_cast<PropertyXLinkSub&>(*copy).copyTo(p->_Links.back());
    for (++it; it != _Links.end(); ++it) {
        p->_Links.emplace_back();
        copy.reset(it->CopyOnLabelChange(obj, ref, newLabel));
        if (copy) {
            static_cast<PropertyXLinkSub&>(*copy).copyTo(p->_Links.back());
        }
        else {
            it->copyTo(p->_Links.back());
        }
    }
    return p.release();
}

Property* PropertyXLinkSubList::CopyOnLinkReplace(const App::DocumentObject* parent,
                                                  App::DocumentObject* oldObj,
                                                  App::DocumentObject* newObj) const
{
    std::unique_ptr<Property> copy;
    PropertyXLinkSub* copied = nullptr;
    std::set<std::string> subs;
    auto it = _Links.begin();
    for (; it != _Links.end(); ++it) {
        copy.reset(it->CopyOnLinkReplace(parent, oldObj, newObj));
        if (copy) {
            copied = static_cast<PropertyXLinkSub*>(copy.get());
            if (copied->getValue() == newObj) {
                for (auto& sub : copied->getSubValues()) {
                    subs.insert(sub);
                }
            }
            break;
        }
    }
    if (!copy) {
        return nullptr;
    }
    std::unique_ptr<PropertyXLinkSubList> p(new PropertyXLinkSubList);
    for (auto iter = _Links.begin(); iter != it; ++iter) {
        if (iter->getValue() == newObj && copied->getValue() == newObj) {
            // merge subnames in case new object already exists
            for (auto& sub : iter->getSubValues()) {
                if (subs.insert(sub).second) {
                    copied->_SubList.push_back(sub);
                }
            }
        }
        else {
            p->_Links.emplace_back();
            iter->copyTo(p->_Links.back());
        }
    }
    p->_Links.emplace_back();
    copied->copyTo(p->_Links.back());
    copied = &p->_Links.back();
    for (++it; it != _Links.end(); ++it) {
        if ((it->getValue() == newObj || it->getValue() == oldObj)
            && copied->getValue() == newObj) {
            // merge subnames in case new object already exists
            for (auto& sub : it->getSubValues()) {
                if (subs.insert(sub).second) {
                    copied->_SubList.push_back(sub);
                }
            }
            continue;
        }
        p->_Links.emplace_back();
        copy.reset(it->CopyOnLinkReplace(parent, oldObj, newObj));
        if (copy) {
            static_cast<PropertyXLinkSub&>(*copy).copyTo(p->_Links.back());
        }
        else {
            it->copyTo(p->_Links.back());
        }
    }
    return p.release();
}

Property* PropertyXLinkSubList::Copy() const
{
    PropertyXLinkSubList* p = new PropertyXLinkSubList();
    for (auto& l : _Links) {
        p->_Links.emplace_back(testFlag(LinkAllowPartial), p);
        l.copyTo(p->_Links.back());
    }
    return p;
}

void PropertyXLinkSubList::Paste(const Property& from)
{
    if (!from.isDerivedFrom<PropertyXLinkSubList>()) {
        throw Base::TypeError("Incompatible property to paste to");
    }

    aboutToSetValue();
    _Links.clear();
    for (auto& l : static_cast<const PropertyXLinkSubList&>(from)._Links) {
        _Links.emplace_back(testFlag(LinkAllowPartial), this);
        _Links.back().Paste(l);
    }
    hasSetValue();
}

unsigned int PropertyXLinkSubList::getMemSize() const
{
    unsigned int size = 0;
    for (auto& l : _Links) {
        size += l.getMemSize();
    }
    return size;
}

const std::vector<std::string>& PropertyXLinkSubList::getSubValues(App::DocumentObject* obj) const
{
    for (auto& l : _Links) {
        if (l.getValue() == obj) {
            return l.getSubValues();
        }
    }
    FC_THROWM(Base::RuntimeError, "object not found");
}

std::vector<std::string> PropertyXLinkSubList::getSubValues(App::DocumentObject* obj,
                                                            bool newStyle) const
{
    for (auto& l : _Links) {
        if (l.getValue() == obj) {
            return l.getSubValues(newStyle);
        }
    }
    return {};
}

void PropertyXLinkSubList::getLinks(std::vector<App::DocumentObject*>& objs,
                                    bool all,
                                    std::vector<std::string>* subs,
                                    bool newStyle) const
{
    if (all || _pcScope != LinkScope::Hidden) {
        if (!subs) {
            objs.reserve(objs.size() + _Links.size());
            for (auto& l : _Links) {
                auto obj = l.getValue();
                if (obj && obj->isAttachedToDocument()) {
                    objs.push_back(obj);
                }
            }
            return;
        }
        size_t count = 0;
        for (auto& l : _Links) {
            auto obj = l.getValue();
            if (obj && obj->isAttachedToDocument()) {
                count += std::max((int)l.getSubValues().size(), 1);
            }
        }
        if (!count) {
            objs.reserve(objs.size() + _Links.size());
            for (auto& l : _Links) {
                auto obj = l.getValue();
                if (obj && obj->isAttachedToDocument()) {
                    objs.push_back(obj);
                }
            }
            return;
        }

        objs.reserve(objs.size() + count);
        subs->reserve(subs->size() + count);
        for (auto& l : _Links) {
            auto obj = l.getValue();
            if (obj && obj->isAttachedToDocument()) {
                auto subnames = l.getSubValues(newStyle);
                if (subnames.empty()) {
                    subnames.emplace_back("");
                }
                for (auto& sub : subnames) {
                    objs.push_back(obj);
                    subs->push_back(std::move(sub));
                }
            }
        }
    }
}

// Same algorithm as _getLinksTo above, but returns all matches
void PropertyXLinkSubList::_getLinksToList(
    std::vector<App::ObjectIdentifier>& identifiers,
    App::DocumentObject* obj,
    const char* subname,
    const std::vector<std::string>& subs,
    const std::vector<PropertyLinkBase::ShadowSub>& shadows) const
{
    if (!subname) {
        identifiers.emplace_back(*this);
        return;
    }
    App::SubObjectT objT(obj, subname);
    auto subObject = objT.getSubObject();
    auto subElement = objT.getOldElementName();

    int i = -1;
    for (const auto& sub : subs) {
        ++i;
        if (sub == subname) {
            identifiers.emplace_back(*this, i);
            continue;
        }
        if (!subObject) {
            continue;
        }
        // There is a subobject and the subname doesn't match our current entry
        App::SubObjectT sobjT(obj, sub.c_str());
        if (sobjT.getSubObject() == subObject && sobjT.getOldElementName() == subElement) {
            identifiers.emplace_back(*this, i);
            continue;
        }
        // The oldElementName ( short, I.E. "Edge5" ) doesn't match.
        if (i < (int)shadows.size()) {
            const auto& [shadowNewName, shadowOldName] = shadows[i];
            if (shadowNewName == subname || shadowOldName == subname) {
                identifiers.emplace_back(*this, i);
                continue;
            }
            if (!subObject) {
                continue;
            }
            App::SubObjectT shadowobjT(obj,
                                       shadowNewName.empty() ? shadowOldName.c_str()
                                                             : shadowNewName.c_str());
            if (shadowobjT.getSubObject() == subObject
                && shadowobjT.getOldElementName() == subElement) {
                identifiers.emplace_back(*this, i);
                continue;
            }
        }
    }
}

void PropertyXLinkSubList::getLinksTo(std::vector<App::ObjectIdentifier>& identifiers,
                                      App::DocumentObject* obj,
                                      const char* subname,
                                      bool all) const
{
    if (!all && _pcScope != LinkScope::Hidden) {
        return;
    }
    for (auto& l : _Links) {
        if (obj && obj == l._pcLink) {
            _getLinksToList(identifiers, obj, subname, l._SubList, l._ShadowSubList);
        }
    }
}

void PropertyXLinkSubList::breakLink(App::DocumentObject* obj, bool clear)
{
    if (clear && getContainer() == obj) {
        setValue(nullptr);
        return;
    }
    atomic_change guard(*this, false);
    for (auto& l : _Links) {
        if (l.getValue() == obj) {
            guard.aboutToChange();
            l.setValue(nullptr);
        }
    }
    guard.tryInvoke();
}

bool PropertyXLinkSubList::adjustLink(const std::set<App::DocumentObject*>& inList)
{
    if (_pcScope == LinkScope::Hidden) {
        return false;
    }
    std::map<App::DocumentObject*, std::vector<std::string>> values;
    bool touched = false;
    int count = 0;
    for (auto& l : _Links) {
        auto obj = l.getValue();
        if (!obj || !obj->isAttachedToDocument()) {
            ++count;
            continue;
        }
        if (inList.count(obj) && adjustLinkSubs(this, inList, obj, l._SubList, &values)) {
            touched = true;
        }
    }
    if (touched) {
        decltype(_Links) tmp;
        if (count) {
            // XLink allows detached state, i.e. with closed external document. So
            // we need to preserve empty link
            for (auto it = _Links.begin(), itNext = it; it != _Links.end(); it = itNext) {
                ++itNext;
                if (!it->getValue()) {
                    tmp.splice(tmp.end(), _Links, it);
                }
            }
        }
        setValues(std::move(values));
        _Links.splice(_Links.end(), tmp);
    }
    return touched;
}

int PropertyXLinkSubList::checkRestore(std::string* msg) const
{
    for (auto& l : _Links) {
        int res;
        if ((res = l.checkRestore(msg))) {
            return res;
        }
    }
    return 0;
}

bool PropertyXLinkSubList::upgrade(Base::XMLReader& reader, const char* typeName)
{
    if (strcmp(typeName, PropertyLinkListGlobal::getClassTypeId().getName()) == 0
        || strcmp(typeName, PropertyLinkList::getClassTypeId().getName()) == 0
        || strcmp(typeName, PropertyLinkListChild::getClassTypeId().getName()) == 0) {
        PropertyLinkList linkProp;
        linkProp.setContainer(getContainer());
        linkProp.Restore(reader);
        setValues(linkProp.getValues());
        return true;
    }
    else if (strcmp(typeName, PropertyLinkSubListGlobal::getClassTypeId().getName()) == 0
             || strcmp(typeName, PropertyLinkSubList::getClassTypeId().getName()) == 0
             || strcmp(typeName, PropertyLinkSubListChild::getClassTypeId().getName()) == 0) {
        PropertyLinkSubList linkProp;
        linkProp.setContainer(getContainer());
        linkProp.Restore(reader);
        std::map<DocumentObject*, std::vector<std::string>> values;
        const auto& objs = linkProp.getValues();
        const auto& subs = linkProp.getSubValues();
        assert(objs.size() == subs.size());
        for (size_t i = 0; i < objs.size(); ++i) {
            values[objs[i]].push_back(subs[i]);
        }
        setValues(std::move(values));
        return true;
    }
    _Links.clear();
    _Links.emplace_back(testFlag(LinkAllowPartial), this);
    if (!_Links.back().upgrade(reader, typeName)) {
        _Links.clear();
        return false;
    }
    return true;
}

void PropertyXLinkSubList::setAllowPartial(bool enable)
{
    setFlag(LinkAllowPartial, enable);
    for (auto& l : _Links) {
        l.setAllowPartial(enable);
    }
}

void PropertyXLinkSubList::hasSetChildValue(Property&)
{
    if (!signalCounter) {
        hasSetValue();
    }
}

void PropertyXLinkSubList::aboutToSetChildValue(Property&)
{
    if (!signalCounter || !hasChanged) {
        aboutToSetValue();
        if (signalCounter) {
            hasChanged = true;
        }
    }
}

std::vector<App::DocumentObject*> PropertyXLinkSubList::getValues() const
{
    std::vector<DocumentObject*> xLinks;
    getLinks(xLinks);
    return (xLinks);
}

//**************************************************************************
// PropertyXLinkList
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

TYPESYSTEM_SOURCE(App::PropertyXLinkList, App::PropertyXLinkSubList)

//**************************************************************************
// Construction/Destruction

PropertyXLinkList::PropertyXLinkList() = default;

PropertyXLinkList::~PropertyXLinkList() = default;

PyObject* PropertyXLinkList::getPyObject()
{
    for (auto& link : _Links) {
        auto obj = link.getValue();
        if (!obj || !obj->isAttachedToDocument()) {
            continue;
        }
        if (link.hasSubName()) {
            return PropertyXLinkSubList::getPyObject();
        }
    }

    Py::List list;
    for (auto& link : _Links) {
        auto obj = link.getValue();
        if (!obj || !obj->isAttachedToDocument()) {
            continue;
        }
        list.append(Py::asObject(obj->getPyObject()));
    }
    return Py::new_reference_to(list);
}

void PropertyXLinkList::setPyObject(PyObject* value)
{
    try {  // try PropertyLinkList syntax
        PropertyLinkList dummy;
        dummy.setAllowExternal(true);
        dummy.setPyObject(value);
        this->setValues(dummy.getValues());
        return;
    }
    catch (Base::Exception&) {
    }

    PropertyXLinkSubList::setPyObject(value);
}

//**************************************************************************
// PropertyXLinkContainer
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
TYPESYSTEM_SOURCE_ABSTRACT(App::PropertyXLinkContainer, App::PropertyLinkBase)

PropertyXLinkContainer::PropertyXLinkContainer()
{
    _pcScope = LinkScope::Global;
    _LinkRestored = false;
}

PropertyXLinkContainer::~PropertyXLinkContainer() = default;

void PropertyXLinkContainer::afterRestore()
{
    _DocMap.clear();
    if (!_XLinkRestores) {
        return;
    }
    _Deps.clear();
    for (auto& info : *_XLinkRestores) {
        auto obj = info.xlink->getValue();
        if (!obj) {
            continue;
        }
        if (!info.docName.empty()) {
            if (info.docName != obj->getDocument()->getName()) {
                _DocMap[info.docName] = obj->getDocument()->getName();
            }
            if (info.docLabel != obj->getDocument()->Label.getValue()) {
                _DocMap[App::quote(info.docLabel)] = obj->getDocument()->Label.getValue();
            }
        }
        if (_Deps.insert(std::make_pair(obj, info.xlink->getScope() == LinkScope::Hidden)).second) {
            _XLinks[obj->getFullName()] = std::move(info.xlink);
            onAddDep(obj);
        }
    }
    _XLinkRestores.reset();
}

void PropertyXLinkContainer::breakLink(App::DocumentObject* obj, bool clear)
{
    if (!obj || !obj->isAttachedToDocument()) {
        return;
    }
    auto owner = dynamic_cast<App::DocumentObject*>(getContainer());
    if (!owner || !owner->isAttachedToDocument()) {
        return;
    }
    if (!clear || obj != owner) {
        auto it = _Deps.find(obj);
        if (it == _Deps.end()) {
            return;
        }
        aboutToSetValue();
        onBreakLink(obj);
        if (obj->getDocument() != owner->getDocument()) {
            _XLinks.erase(obj->getFullName());
        }
        else if (!it->second) {
            obj->_removeBackLink(owner);
        }
        _Deps.erase(it);
        onRemoveDep(obj);
        hasSetValue();
        return;
    }
    if (obj != owner) {
        return;
    }
    for (auto& v : _Deps) {
        auto key = v.first;
        if (!key || !key->isAttachedToDocument()) {
            continue;
        }
        onBreakLink(key);
        if (!v.second && key->getDocument() == owner->getDocument()) {
            key->_removeBackLink(owner);
        }
    }
    _XLinks.clear();
    _Deps.clear();
}

int PropertyXLinkContainer::checkRestore(std::string* msg) const
{
    if (_LinkRestored) {
        for (auto& v : _XLinks) {
            int res = v.second->checkRestore(msg);
            if (res) {
                return res;
            }
        }
    }
    return 0;
}

void PropertyXLinkContainer::Save(Base::Writer& writer) const
{

    writer.Stream() << writer.ind() << "<XLinks count=\"" << _XLinks.size();

    std::map<App::Document*, int> docSet;
    auto owner = Base::freecad_dynamic_cast<App::DocumentObject>(getContainer());
    if (owner && !owner->isExporting()) {
        // Document name and label can change on restore, we shall record the
        // current document name and label and pair it with the associated
        // xlink, so that we can restore them correctly.
        int i = -1;
        for (auto& v : _XLinks) {
            ++i;
            auto obj = v.second->getValue();
            if (obj && obj->getDocument()) {
                docSet.insert(std::make_pair(obj->getDocument(), i));
            }
        }

        if (!docSet.empty()) {
            writer.Stream() << "\" docs=\"" << docSet.size();
        }
    }

    std::ostringstream ss;
    int hidden = 0;
    int i = -1;
    for (auto& v : _XLinks) {
        ++i;
        if (v.second->getScope() == LinkScope::Hidden) {
            ss << i << ' ';
            ++hidden;
        }
    }
    if (hidden) {
        writer.Stream() << "\" hidden=\"" << ss.str();
    }

    writer.Stream() << "\">" << std::endl;
    writer.incInd();

    for (auto& v : docSet) {
        writer.Stream() << writer.ind() << "<DocMap "
                        << "name=\"" << v.first->getName() << "\" label=\""
                        << encodeAttribute(v.first->Label.getValue()) << "\" index=\"" << v.second
                        << "\"/>" << std::endl;
    }

    for (auto& v : _XLinks) {
        v.second->Save(writer);
    }
    writer.decInd();

    writer.Stream() << writer.ind() << "</XLinks>" << std::endl;
}

void PropertyXLinkContainer::Restore(Base::XMLReader& reader)
{
    reader.readElement("XLinks");
    auto count = reader.getAttributeAsUnsigned("count");
    _XLinkRestores = std::make_unique<std::vector<RestoreInfo>>(count);

    if (reader.hasAttribute("hidden")) {
        std::istringstream iss(reader.getAttribute("hidden"));
        int index;
        while (iss >> index) {
            if (index >= 0 && index < static_cast<int>(count)) {
                _XLinkRestores->at(index).hidden = true;
            }
        }
    }

    if (reader.hasAttribute("docs")) {
        auto docCount = reader.getAttributeAsUnsigned("docs");
        _DocMap.clear();
        for (unsigned i = 0; i < docCount; ++i) {
            reader.readElement("DocMap");
            auto index = reader.getAttributeAsUnsigned("index");
            if (index >= count) {
                FC_ERR(propertyName(this) << " invalid document map entry");
                continue;
            }
            auto& info = _XLinkRestores->at(index);
            info.docName = reader.getAttribute("name");
            info.docLabel = reader.getAttribute("label");
        }
    }

    for (auto& info : *_XLinkRestores) {
        info.xlink.reset(createXLink());
        if (info.hidden) {
            info.xlink->setScope(LinkScope::Hidden);
        }
        info.xlink->Restore(reader);
    }
    reader.readEndElement("XLinks");
}

void PropertyXLinkContainer::aboutToSetChildValue(App::Property& prop)
{
    auto xlink = dynamic_cast<App::PropertyXLink*>(&prop);
    if (xlink && xlink->testFlag(LinkDetached)) {
        auto obj = const_cast<App::DocumentObject*>(xlink->getValue());
        if (_Deps.erase(obj)) {
            _onBreakLink(xlink->getValue());
            onRemoveDep(obj);
        }
    }
}

void PropertyXLinkContainer::onBreakLink(DocumentObject*)
{}

void PropertyXLinkContainer::_onBreakLink(DocumentObject* obj)
{
    try {
        onBreakLink(obj);
    }
    catch (Base::Exception& e) {
        e.ReportException();
        FC_ERR("Exception on breaking link property " << getFullName());
    }
    catch (std::exception& e) {
        FC_ERR("Exception on breaking link property " << getFullName() << ": " << e.what());
    }
    catch (...) {
        FC_ERR("Exception on breaking link property " << getFullName());
    }
}

PropertyXLink* PropertyXLinkContainer::createXLink()
{
    return new PropertyXLink(false, this);
}

bool PropertyXLinkContainer::isLinkedToDocument(const App::Document& doc) const
{
    auto iter = _XLinks.lower_bound(doc.getName());
    if (iter != _XLinks.end()) {
        size_t len = strlen(doc.getName());
        return iter->first.size() > len && iter->first[len] == '#'
            && boost::starts_with(iter->first, doc.getName());
    }
    return false;
}

void PropertyXLinkContainer::updateDeps(std::map<DocumentObject*, bool>&& newDeps)
{
    auto owner = Base::freecad_dynamic_cast<App::DocumentObject>(getContainer());
    if (!owner || !owner->isAttachedToDocument()) {
        return;
    }
    newDeps.erase(owner);

    for (auto& v : newDeps) {
        auto obj = v.first;
        if (obj && obj->isAttachedToDocument()) {
            auto it = _Deps.find(obj);
            if (it != _Deps.end()) {
                if (v.second != it->second) {
                    if (v.second) {
                        obj->_removeBackLink(owner);
                    }
                    else {
                        obj->_addBackLink(owner);
                    }
                }
                _Deps.erase(it);
                continue;
            }
            if (owner->getDocument() != obj->getDocument()) {
                auto& xlink = _XLinks[obj->getFullName()];
                if (!xlink) {
                    xlink.reset(createXLink());
                    xlink->setValue(obj);
                }
                xlink->setScope(v.second ? LinkScope::Hidden : LinkScope::Global);
            }
            else if (!v.second) {
                obj->_addBackLink(owner);
            }

            onAddDep(obj);
        }
    }
    for (auto& v : _Deps) {
        auto obj = v.first;
        if (!obj || !obj->isAttachedToDocument()) {
            continue;
        }
        if (obj->getDocument() == owner->getDocument()) {
            if (!v.second) {
                obj->_removeBackLink(owner);
            }
        }
        else {
            _XLinks.erase(obj->getFullName());
        }
        onRemoveDep(obj);
    }
    _Deps = std::move(newDeps);

    _LinkRestored = testFlag(LinkRestoring);

    if (!_LinkRestored && !testFlag(LinkDetached)) {
        for (auto it = _XLinks.begin(), itNext = it; it != _XLinks.end(); it = itNext) {
            ++itNext;
            if (!it->second->getValue()) {
                _XLinks.erase(it);
            }
        }
    }
}

void PropertyXLinkContainer::clearDeps()
{
    auto owner = dynamic_cast<App::DocumentObject*>(getContainer());
    if (!owner || !owner->isAttachedToDocument()) {
        return;
    }

    if (!owner->testStatus(ObjectStatus::Destroy)) {
        for (auto& v : _Deps) {
            auto obj = v.first;
            if (!v.second && obj && obj->isAttachedToDocument()
                && obj->getDocument() == owner->getDocument()) {
                obj->_removeBackLink(owner);
            }
        }
    }

    _Deps.clear();
    _XLinks.clear();
    _LinkRestored = false;
}

void PropertyXLinkContainer::getLinks(std::vector<App::DocumentObject*>& objs,
                                      bool all,
                                      std::vector<std::string>* /*subs*/,
                                      bool /*newStyle*/) const
{
    for (auto& v : _Deps) {
        if (all || !v.second) {
            objs.push_back(v.first);
        }
    }
}
