/**
 * (C) 2017 KISTLER INSTRUMENTE AG, Winterthur, Switzerland
 *
 * @file Object.cpp
 *
 * Created on: Nov 8, 2016
 * Project: sdbus-c++
 * Description: High-level D-Bus IPC C++ library based on sd-bus
 *
 * This file is part of sdbus-c++.
 *
 * sdbus-c++ is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * sdbus-c++ is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with sdbus-c++. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Object.h"
#include <sdbus-c++/IConnection.h>
#include <sdbus-c++/Message.h>
#include <sdbus-c++/Error.h>
#include "IConnection.h"
#include "VTableUtils.h"
#include <systemd/sd-bus.h>
#include <utility>
#include <cassert>

namespace sdbus { namespace internal {

Object::Object(sdbus::internal::IConnection& connection, std::string objectPath)
    : connection_(connection), objectPath_(std::move(objectPath))
{
}

void Object::registerMethod( const std::string& interfaceName
                           , const std::string& methodName
                           , const std::string& inputSignature
                           , const std::string& outputSignature
                           , method_callback methodCallback )
{
    SDBUS_THROW_ERROR_IF(!methodCallback, "Invalid method callback provided", EINVAL);

    auto& interface = interfaces_[interfaceName];

    InterfaceData::MethodData methodData{inputSignature, outputSignature, std::move(methodCallback)};
    auto inserted = interface.methods_.emplace(methodName, std::move(methodData)).second;

    SDBUS_THROW_ERROR_IF(!inserted, "Failed to register method: method already exists", EINVAL);
}

void Object::registerSignal( const std::string& interfaceName
                           , const std::string& signalName
                           , const std::string& signature )
{
    auto& interface = interfaces_[interfaceName];

    InterfaceData::SignalData signalData{signature};
    auto inserted = interface.signals_.emplace(signalName, std::move(signalData)).second;

    SDBUS_THROW_ERROR_IF(!inserted, "Failed to register signal: signal already exists", EINVAL);
}

void Object::registerProperty( const std::string& interfaceName
                             , const std::string& propertyName
                             , const std::string& signature
                             , property_get_callback getCallback )
{
    registerProperty( interfaceName
                    , propertyName
                    , signature
                    , getCallback
                    , property_set_callback{} );
}

void Object::registerProperty( const std::string& interfaceName
                             , const std::string& propertyName
                             , const std::string& signature
                             , property_get_callback getCallback
                             , property_set_callback setCallback )
{
    SDBUS_THROW_ERROR_IF(!getCallback && !setCallback, "Invalid property callbacks provided", EINVAL);

    auto& interface = interfaces_[interfaceName];

    InterfaceData::PropertyData propertyData{signature, std::move(getCallback), std::move(setCallback)};
    auto inserted = interface.properties_.emplace(propertyName, std::move(propertyData)).second;

    SDBUS_THROW_ERROR_IF(!inserted, "Failed to register property: property already exists", EINVAL);
}

void Object::finishRegistration()
{
    for (auto& item : interfaces_)
    {
        const auto& interfaceName = item.first;
        auto& interfaceData = item.second;

        const auto& vtable = createInterfaceVTable(interfaceData);
        activateInterfaceVTable(interfaceName, interfaceData, vtable);
    }
}

sdbus::Message Object::createSignal(const std::string& interfaceName, const std::string& signalName)
{
    // Tell, don't ask
    return connection_.createSignal(objectPath_, interfaceName, signalName);
}

void Object::emitSignal(const sdbus::Message& message)
{
    message.send();
}

const std::vector<sd_bus_vtable>& Object::createInterfaceVTable(InterfaceData& interfaceData)
{
    auto& vtable = interfaceData.vtable_;
    assert(vtable.empty());

    vtable.push_back(createVTableStartItem());
    registerMethodsToVTable(interfaceData, vtable);
    registerSignalsToVTable(interfaceData, vtable);
    registerPropertiesToVTable(interfaceData, vtable);
    vtable.push_back(createVTableEndItem());

    return vtable;
}

void Object::registerMethodsToVTable(const InterfaceData& interfaceData, std::vector<sd_bus_vtable>& vtable)
{
    for (const auto& item : interfaceData.methods_)
    {
        const auto& methodName = item.first;
        const auto& methodData = item.second;

        vtable.push_back(createVTableMethodItem( methodName.c_str()
                                               , methodData.inputArgs_.c_str()
                                               , methodData.outputArgs_.c_str()
                                               , &Object::sdbus_method_callback ));
    }
}

void Object::registerSignalsToVTable(const InterfaceData& interfaceData, std::vector<sd_bus_vtable>& vtable)
{
    for (const auto& item : interfaceData.signals_)
    {
        const auto& signalName = item.first;
        const auto& signalData = item.second;

        vtable.push_back(createVTableSignalItem( signalName.c_str()
                                               , signalData.signature_.c_str() ));
    }
}

void Object::registerPropertiesToVTable(const InterfaceData& interfaceData, std::vector<sd_bus_vtable>& vtable)
{
    for (const auto& item : interfaceData.properties_)
    {
        const auto& propertyName = item.first;
        const auto& propertyData = item.second;

        if (!propertyData.setCallback_)
            vtable.push_back(createVTablePropertyItem( propertyName.c_str()
                                                     , propertyData.signature_.c_str()
                                                     , &Object::sdbus_property_get_callback ));
        else
            vtable.push_back(createVTableWritablePropertyItem( propertyName.c_str()
                                                             , propertyData.signature_.c_str()
                                                             , &Object::sdbus_property_get_callback
                                                             , &Object::sdbus_property_set_callback ));
    }
}

void Object::activateInterfaceVTable( const std::string& interfaceName
                                    , InterfaceData& interfaceData
                                    , const std::vector<sd_bus_vtable>& vtable )
{
    // Tell, don't ask
    auto slot = (sd_bus_slot*) connection_.addObjectVTable(objectPath_, interfaceName, &vtable[0], this);
    interfaceData.slot_.reset(slot);
    interfaceData.slot_.get_deleter() = [this](void *slot){ connection_.removeObjectVTable(slot); };
}

int Object::sdbus_method_callback(sd_bus_message *sdbusMessage, void *userData, sd_bus_error *retError)
{
    Message message(sdbusMessage, Message::Type::eMethodCall);

    auto* object = static_cast<Object*>(userData);
    // Note: The lookup can be optimized by using sorted vectors instead of associative containers
    auto& callback = object->interfaces_[message.getInterfaceName()].methods_[message.getMemberName()].callback_;
    assert(callback);

    auto reply = message.createReply();

    try
    {
        callback(message, reply);
    }
    catch (const sdbus::Error& e)
    {
        sd_bus_error_set(retError, e.getName().c_str(), e.getMessage().c_str());
        return 1;
    }

    reply.send();

    return 1;
}

int Object::sdbus_property_get_callback( sd_bus */*bus*/
                                       , const char */*objectPath*/
                                       , const char *interface
                                       , const char *property
                                       , sd_bus_message *sdbusReply
                                       , void *userData
                                       , sd_bus_error *retError )
{
    Message reply(sdbusReply, Message::Type::ePlainMessage);

    auto* object = static_cast<Object*>(userData);
    // Note: The lookup can be optimized by using sorted vectors instead of associative containers
    auto& callback = object->interfaces_[interface].properties_[property].getCallback_;
    // Getter can be empty - the case of "write-only" property
    if (!callback)
    {
        sd_bus_error_set(retError, "org.freedesktop.DBus.Error.Failed", "Cannot read property as it is write-only");
        return 1;
    }

    try
    {
        callback(reply);
    }
    catch (const sdbus::Error& e)
    {
        sd_bus_error_set(retError, e.getName().c_str(), e.getMessage().c_str());
    }

    return 1;
}

int Object::sdbus_property_set_callback( sd_bus */*bus*/
                                       , const char */*objectPath*/
                                       , const char *interface
                                       , const char *property
                                       , sd_bus_message *sdbusValue
                                       , void *userData
                                       , sd_bus_error *retError )
{
    Message value(sdbusValue, Message::Type::ePlainMessage);

    auto* object = static_cast<Object*>(userData);
    // Note: The lookup can be optimized by using sorted vectors instead of associative containers
    auto& callback = object->interfaces_[interface].properties_[property].setCallback_;
    assert(callback);

    try
    {
        callback(value);
    }
    catch (const sdbus::Error& e)
    {
        sd_bus_error_set(retError, e.getName().c_str(), e.getMessage().c_str());
        return 1;
    }

    return 1;
}

}}

namespace sdbus {

std::unique_ptr<sdbus::IObject> createObject(sdbus::IConnection& connection, std::string objectPath)
{
    auto* sdbusConnection = dynamic_cast<sdbus::internal::IConnection*>(&connection);
    SDBUS_THROW_ERROR_IF(!sdbusConnection, "Connection is not a real sdbus-c++ connection", EINVAL);

    return std::make_unique<sdbus::internal::Object>(*sdbusConnection, std::move(objectPath));
}

}
