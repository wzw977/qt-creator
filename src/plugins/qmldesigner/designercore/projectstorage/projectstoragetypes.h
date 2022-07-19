/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#pragma once

#include "filestatus.h"
#include "projectstorageids.h"

#include <utils/smallstring.h>
#include <utils/variant.h>

#include <tuple>
#include <vector>

namespace QmlDesigner {

template<typename Enumeration>
constexpr std::underlying_type_t<Enumeration> to_underlying(Enumeration enumeration) noexcept
{
    static_assert(std::is_enum_v<Enumeration>, "to_underlying expect an enumeration");
    return static_cast<std::underlying_type_t<Enumeration>>(enumeration);
}

} // namespace QmlDesigner

namespace QmlDesigner::Storage {
enum class PropertyDeclarationTraits : int {
    None = 0,
    IsReadOnly = 1 << 0,
    IsPointer = 1 << 1,
    IsList = 1 << 2
};
constexpr PropertyDeclarationTraits operator|(PropertyDeclarationTraits first,
                                              PropertyDeclarationTraits second)
{
    return static_cast<PropertyDeclarationTraits>(static_cast<int>(first) | static_cast<int>(second));
}

constexpr bool operator&(PropertyDeclarationTraits first, PropertyDeclarationTraits second)
{
    return static_cast<int>(first) & static_cast<int>(second);
}

using TypeNameString = Utils::BasicSmallString<63>;

} // namespace QmlDesigner::Storage

namespace QmlDesigner::Storage::Synchronization {

enum class TypeAccessSemantics : int { None, Reference, Value, Sequence, IsEnum = 1 << 8 };

constexpr TypeAccessSemantics operator|(TypeAccessSemantics first, TypeAccessSemantics second)
{
    return static_cast<TypeAccessSemantics>(static_cast<int>(first) | static_cast<int>(second));
}

enum class TypeNameKind { Exported = 1, QualifiedExported = 2 };

enum class FileType : char { QmlTypes, QmlDocument };

class VersionNumber
{
public:
    explicit VersionNumber() = default;
    explicit VersionNumber(int value)
        : value{value}
    {}

    explicit operator bool() const { return value >= 0; }

    friend bool operator==(VersionNumber first, VersionNumber second) noexcept
    {
        return first.value == second.value;
    }

    friend bool operator!=(VersionNumber first, VersionNumber second) noexcept
    {
        return !(first == second);
    }

    friend bool operator<(VersionNumber first, VersionNumber second) noexcept
    {
        return first.value < second.value;
    }

public:
    int value = -1;
};

class Version
{
public:
    explicit Version() = default;
    explicit Version(VersionNumber major, VersionNumber minor = VersionNumber{})
        : major{major}
        , minor{minor}
    {}

    explicit Version(int major, int minor)
        : major{major}
        , minor{minor}
    {}

    explicit Version(int major)
        : major{major}
    {}

    friend bool operator==(Version first, Version second) noexcept
    {
        return first.major == second.major && first.minor == second.minor;
    }

    friend bool operator<(Version first, Version second) noexcept
    {
        return std::tie(first.major, first.minor) < std::tie(second.major, second.minor);
    }

    explicit operator bool() const { return major && minor; }

public:
    VersionNumber major;
    VersionNumber minor;
};

enum class IsQualified : int { No, Yes };

inline int operator-(IsQualified first, IsQualified second)
{
    return static_cast<int>(first) - static_cast<int>(second);
}

inline int operator<(IsQualified first, IsQualified second)
{
    return static_cast<int>(first) < static_cast<int>(second);
}

enum class ImportKind : char {
    Import,
    ModuleDependency,
    ModuleExportedImport,
    ModuleExportedModuleDependency
};

class Import
{
public:
    explicit Import() = default;

    explicit Import(ModuleId moduleId, Version version, SourceId sourceId)
        : version{version}
        , moduleId{moduleId}
        , sourceId{sourceId}
    {}

    explicit Import(int moduleId, int majorVersion, int minorVersion, int sourceId)
        : version{majorVersion, minorVersion}
        , moduleId{moduleId}
        , sourceId{sourceId}
    {}

    friend bool operator==(const Import &first, const Import &second)
    {
        return first.moduleId == second.moduleId && first.version == second.version
               && first.sourceId == second.sourceId;
    }

    friend bool operator<(const Import &first, const Import &second)
    {
        return std::tie(first.sourceId, first.moduleId, first.version)
               < std::tie(second.sourceId, second.moduleId, second.version);
    }

public:
    Version version;
    ModuleId moduleId;
    SourceId sourceId;
};

using Imports = std::vector<Import>;

class ImportView
{
public:
    explicit ImportView() = default;

    explicit ImportView(long long importId, int sourceId, int moduleId, int majorVersion, int minorVersion)
        : importId{importId}
        , sourceId{sourceId}
        , moduleId{moduleId}
        , version{majorVersion, minorVersion}
    {}

    friend bool operator==(const ImportView &first, const ImportView &second)
    {
        return first.sourceId == second.sourceId && first.moduleId == second.moduleId
               && first.version == second.version;
    }

public:
    ImportId importId;
    SourceId sourceId;
    ModuleId moduleId;
    Version version;
};

enum class IsAutoVersion : char { No, Yes };

constexpr bool operator<(IsAutoVersion first, IsAutoVersion second)
{
    return to_underlying(first) < to_underlying(second);
}

class ModuleExportedImport
{
public:
    explicit ModuleExportedImport(ModuleId moduleId,
                                  ModuleId exportedModuleId,
                                  Version version,
                                  IsAutoVersion isAutoVersion)
        : version{version}
        , moduleId{moduleId}
        , exportedModuleId{exportedModuleId}
        , isAutoVersion{isAutoVersion}
    {}

    friend bool operator==(const ModuleExportedImport &first, const ModuleExportedImport &second)
    {
        return first.moduleId == second.moduleId && first.version == second.version
               && first.exportedModuleId == second.exportedModuleId
               && first.isAutoVersion == second.isAutoVersion;
    }

    friend bool operator<(const ModuleExportedImport &first, const ModuleExportedImport &second)
    {
        return std::tie(first.moduleId, first.exportedModuleId, first.isAutoVersion, first.version)
               < std::tie(second.moduleId, second.exportedModuleId, second.isAutoVersion, second.version);
    }

public:
    Version version;
    ModuleId moduleId;
    ModuleId exportedModuleId;
    IsAutoVersion isAutoVersion = IsAutoVersion::No;
};

using ModuleExportedImports = std::vector<ModuleExportedImport>;

class ModuleExportedImportView
{
public:
    explicit ModuleExportedImportView() = default;

    explicit ModuleExportedImportView(long long moduleExportedImportId,
                                      int moduleId,
                                      int exportedModuleId,
                                      int majorVersion,
                                      int minorVersion,
                                      int isAutoVersion)
        : moduleExportedImportId{moduleExportedImportId}
        , version{majorVersion, minorVersion}
        , moduleId{moduleId}
        , exportedModuleId{exportedModuleId}
        , isAutoVersion{static_cast<IsAutoVersion>(isAutoVersion)}
    {}

    friend bool operator==(const ModuleExportedImportView &first,
                           const ModuleExportedImportView &second)
    {
        return first.moduleId == second.moduleId && first.exportedModuleId == second.exportedModuleId
               && first.version == second.version && first.isAutoVersion == second.isAutoVersion;
    }

public:
    ModuleExportedImportId moduleExportedImportId;
    Version version;
    ModuleId moduleId;
    ModuleId exportedModuleId;
    IsAutoVersion isAutoVersion = IsAutoVersion::No;
};

class ImportedType
{
public:
    explicit ImportedType() = default;
    explicit ImportedType(Utils::SmallStringView name)
        : name{name}
    {}

    friend bool operator==(const ImportedType &first, const ImportedType &second)
    {
        return first.name == second.name;
    }

public:
    TypeNameString name;
};

class QualifiedImportedType
{
public:
    explicit QualifiedImportedType() = default;
    explicit QualifiedImportedType(Utils::SmallStringView name, Import import)
        : name{name}
        , import{std::move(import)}
    {}

    friend bool operator==(const QualifiedImportedType &first, const QualifiedImportedType &second)
    {
        return first.name == second.name && first.import == second.import;
    }

public:
    TypeNameString name;
    Import import;
};

using ImportedTypes = std::vector<ImportedType>;

class ExportedType
{
public:
    explicit ExportedType() = default;
    explicit ExportedType(Utils::SmallStringView name, Version version = Version{})
        : name{name}
        , version{version}
    {}

    explicit ExportedType(ModuleId moduleId, Utils::SmallStringView name, Version version = Version{})
        : name{name}
        , version{version}
        , moduleId{moduleId}
    {}

    explicit ExportedType(Utils::SmallStringView name, Version version, TypeId typeId, ModuleId moduleId)
        : name{name}
        , version{version}
        , typeId{typeId}
        , moduleId{moduleId}
    {}

    explicit ExportedType(int moduleId, Utils::SmallStringView name, int majorVersion, int minorVersion)
        : name{name}
        , version{majorVersion, minorVersion}
        , moduleId{moduleId}
    {}

    friend bool operator==(const ExportedType &first, const ExportedType &second)
    {
        return first.name == second.name;
    }

    friend bool operator<(const ExportedType &first, const ExportedType &second)
    {
        return std::tie(first.moduleId, first.name, first.version)
               < std::tie(second.moduleId, second.name, second.version);
    }

public:
    Utils::SmallString name;
    Version version;
    TypeId typeId;
    ModuleId moduleId;
};

using ExportedTypes = std::vector<ExportedType>;

class ExportedTypeView
{
public:
    explicit ExportedTypeView() = default;
    explicit ExportedTypeView(ModuleId moduleId, Utils::SmallStringView name, Version version)
        : name{name}
        , version{version}
        , moduleId{moduleId}
    {}
    explicit ExportedTypeView(int moduleId,
                              Utils::SmallStringView name,
                              int majorVersion,
                              int minorVersion,
                              int typeId,
                              long long exportedTypeNameId)
        : name{name}
        , version{majorVersion, minorVersion}
        , typeId{typeId}
        , moduleId{moduleId}
        , exportedTypeNameId{exportedTypeNameId}
    {}

public:
    Utils::SmallStringView name;
    Version version;
    TypeId typeId;
    ModuleId moduleId;
    ExportedTypeNameId exportedTypeNameId;
};

using ImportedTypeName = Utils::variant<ImportedType, QualifiedImportedType>;

class EnumeratorDeclaration
{
public:
    explicit EnumeratorDeclaration() = default;
    explicit EnumeratorDeclaration(Utils::SmallStringView name, long long value, int hasValue = true)
        : name{name}
        , value{value}
        , hasValue{bool(hasValue)}
    {}

    explicit EnumeratorDeclaration(Utils::SmallStringView name)
        : name{name}
    {}

    friend bool operator==(const EnumeratorDeclaration &first, const EnumeratorDeclaration &second)
    {
        return first.name == second.name && first.value == second.value
               && first.hasValue == second.hasValue;
    }

public:
    Utils::SmallString name;
    long long value = 0;
    bool hasValue = false;
};

using EnumeratorDeclarations = std::vector<EnumeratorDeclaration>;

class EnumerationDeclaration
{
public:
    explicit EnumerationDeclaration() = default;
    explicit EnumerationDeclaration(Utils::SmallStringView name,
                                    EnumeratorDeclarations enumeratorDeclarations)
        : name{name}
        , enumeratorDeclarations{std::move(enumeratorDeclarations)}
    {}

    friend bool operator==(const EnumerationDeclaration &first, const EnumerationDeclaration &second)
    {
        return first.name == second.name
               && first.enumeratorDeclarations == second.enumeratorDeclarations;
    }

public:
    TypeNameString name;
    EnumeratorDeclarations enumeratorDeclarations;
};

using EnumerationDeclarations = std::vector<EnumerationDeclaration>;

class EnumerationDeclarationView
{
public:
    explicit EnumerationDeclarationView() = default;
    explicit EnumerationDeclarationView(Utils::SmallStringView name,
                                        Utils::SmallStringView enumeratorDeclarations,
                                        long long id)
        : name{name}
        , enumeratorDeclarations{std::move(enumeratorDeclarations)}
        , id{id}
    {}

public:
    Utils::SmallStringView name;
    Utils::SmallStringView enumeratorDeclarations;
    EnumerationDeclarationId id;
};

class ParameterDeclaration
{
public:
    explicit ParameterDeclaration() = default;
    explicit ParameterDeclaration(Utils::SmallStringView name,
                                  Utils::SmallStringView typeName,
                                  PropertyDeclarationTraits traits = {})
        : name{name}
        , typeName{typeName}
        , traits{traits}
    {}

    explicit ParameterDeclaration(Utils::SmallStringView name, Utils::SmallStringView typeName, int traits)
        : name{name}
        , typeName{typeName}
        , traits{static_cast<PropertyDeclarationTraits>(traits)}
    {}

    friend bool operator==(const ParameterDeclaration &first, const ParameterDeclaration &second)
    {
        return first.name == second.name && first.typeName == second.typeName
               && first.traits == second.traits;
    }

public:
    Utils::SmallString name;
    TypeNameString typeName;
    PropertyDeclarationTraits traits = {};
};

using ParameterDeclarations = std::vector<ParameterDeclaration>;

class SignalDeclaration
{
public:
    explicit SignalDeclaration() = default;
    explicit SignalDeclaration(Utils::SmallString name, ParameterDeclarations parameters)
        : name{name}
        , parameters{std::move(parameters)}
    {}

    explicit SignalDeclaration(Utils::SmallString name)
        : name{name}
    {}

    friend bool operator==(const SignalDeclaration &first, const SignalDeclaration &second)
    {
        return first.name == second.name && first.parameters == second.parameters;
    }

public:
    Utils::SmallString name;
    ParameterDeclarations parameters;
};

using SignalDeclarations = std::vector<SignalDeclaration>;

class SignalDeclarationView
{
public:
    explicit SignalDeclarationView() = default;
    explicit SignalDeclarationView(Utils::SmallStringView name,
                                   Utils::SmallStringView signature,
                                   long long id)
        : name{name}
        , signature{signature}
        , id{id}
    {}

public:
    Utils::SmallStringView name;
    Utils::SmallStringView signature;
    SignalDeclarationId id;
};

class FunctionDeclaration
{
public:
    explicit FunctionDeclaration() = default;
    explicit FunctionDeclaration(Utils::SmallStringView name,
                                 Utils::SmallStringView returnTypeName,
                                 ParameterDeclarations parameters)
        : name{name}
        , returnTypeName{returnTypeName}
        , parameters{std::move(parameters)}
    {}

    explicit FunctionDeclaration(Utils::SmallStringView name,
                                 Utils::SmallStringView returnTypeName = {})
        : name{name}
        , returnTypeName{returnTypeName}
    {}

    friend bool operator==(const FunctionDeclaration &first, const FunctionDeclaration &second)
    {
        return first.name == second.name && first.returnTypeName == second.returnTypeName
               && first.parameters == second.parameters;
    }

public:
    Utils::SmallString name;
    TypeNameString returnTypeName;
    ParameterDeclarations parameters;
};

using FunctionDeclarations = std::vector<FunctionDeclaration>;

class FunctionDeclarationView
{
public:
    explicit FunctionDeclarationView() = default;
    explicit FunctionDeclarationView(Utils::SmallStringView name,
                                     Utils::SmallStringView returnTypeName,
                                     Utils::SmallStringView signature,
                                     long long id)
        : name{name}
        , returnTypeName{returnTypeName}
        , signature{signature}
        , id{id}
    {}

public:
    Utils::SmallStringView name;
    Utils::SmallStringView returnTypeName;
    Utils::SmallStringView signature;
    FunctionDeclarationId id;
};

enum class PropertyKind { Property, Alias };

class PropertyDeclaration
{
public:
    explicit PropertyDeclaration() = default;
    explicit PropertyDeclaration(Utils::SmallStringView name,
                                 ImportedTypeName typeName,
                                 PropertyDeclarationTraits traits)
        : name{name}
        , typeName{std::move(typeName)}
        , traits{traits}
        , kind{PropertyKind::Property}
    {}

    explicit PropertyDeclaration(Utils::SmallStringView name,
                                 TypeId propertyTypeId,
                                 PropertyDeclarationTraits traits)
        : name{name}
        , traits{traits}
        , propertyTypeId{propertyTypeId}
        , kind{PropertyKind::Property}
    {}

    explicit PropertyDeclaration(Utils::SmallStringView name,
                                 ImportedTypeName typeName,
                                 PropertyDeclarationTraits traits,
                                 Utils::SmallStringView aliasPropertyName,
                                 Utils::SmallStringView aliasPropertyNameTail = {})
        : name{name}
        , typeName{std::move(typeName)}
        , aliasPropertyName{aliasPropertyName}
        , aliasPropertyNameTail{aliasPropertyNameTail}

        , traits{traits}
        , kind{PropertyKind::Property}
    {}

    explicit PropertyDeclaration(Utils::SmallStringView name,
                                 TypeId propertyTypeId,
                                 PropertyDeclarationTraits traits,
                                 Utils::SmallStringView aliasPropertyName,
                                 Utils::SmallStringView aliasPropertyNameTail = {})
        : name{name}
        , aliasPropertyName{aliasPropertyName}
        , aliasPropertyNameTail{aliasPropertyNameTail}
        , traits{traits}
        , propertyTypeId{propertyTypeId}
        , kind{PropertyKind::Property}
    {}

    explicit PropertyDeclaration(Utils::SmallStringView name,
                                 long long propertyTypeId,
                                 int traits,
                                 Utils::SmallStringView aliasPropertyName,
                                 Utils::SmallStringView aliasPropertyNameTail = {})
        : name{name}
        , aliasPropertyName{aliasPropertyName}
        , aliasPropertyNameTail{aliasPropertyNameTail}

        , traits{static_cast<PropertyDeclarationTraits>(traits)}
        , propertyTypeId{propertyTypeId}
        , kind{PropertyKind::Property}
    {}

    explicit PropertyDeclaration(Utils::SmallStringView name,
                                 ImportedTypeName aliasTypeName,
                                 Utils::SmallStringView aliasPropertyName,
                                 Utils::SmallStringView aliasPropertyNameTail = {})
        : name{name}
        , typeName{std::move(aliasTypeName)}
        , aliasPropertyName{aliasPropertyName}
        , aliasPropertyNameTail{aliasPropertyNameTail}

        , kind{PropertyKind::Alias}
    {}

    friend bool operator==(const PropertyDeclaration &first, const PropertyDeclaration &second)
    {
        return first.name == second.name && first.typeName == second.typeName
               && first.aliasPropertyName == second.aliasPropertyName
               && first.aliasPropertyNameTail == second.aliasPropertyNameTail
               && first.traits == second.traits && first.kind == second.kind;
    }

public:
    Utils::SmallString name;
    ImportedTypeName typeName;
    Utils::SmallString aliasPropertyName;
    Utils::SmallString aliasPropertyNameTail;
    PropertyDeclarationTraits traits = {};
    TypeId propertyTypeId;
    TypeId typeId;
    PropertyKind kind = PropertyKind::Property;
};

using PropertyDeclarations = std::vector<PropertyDeclaration>;

class PropertyDeclarationView
{
public:
    explicit PropertyDeclarationView(Utils::SmallStringView name,
                                     int traits,
                                     long long typeId,
                                     long long typeNameId,
                                     long long id,
                                     long long aliasId)
        : name{name}
        , traits{static_cast<PropertyDeclarationTraits>(traits)}
        , typeId{typeId}
        , typeNameId{typeNameId}
        , id{id}
        , aliasId{aliasId}
    {}

public:
    Utils::SmallStringView name;
    PropertyDeclarationTraits traits = {};
    TypeId typeId;
    ImportedTypeNameId typeNameId;
    PropertyDeclarationId id;
    PropertyDeclarationId aliasId;
};

enum class ChangeLevel : char { Full, Minimal, ExcludeExportedTypes };

class Type
{
public:
    explicit Type() = default;
    explicit Type(Utils::SmallStringView typeName,
                  ImportedTypeName prototype,
                  TypeAccessSemantics accessSemantics,
                  SourceId sourceId,
                  ExportedTypes exportedTypes = {},
                  PropertyDeclarations propertyDeclarations = {},
                  FunctionDeclarations functionDeclarations = {},
                  SignalDeclarations signalDeclarations = {},
                  EnumerationDeclarations enumerationDeclarations = {},
                  ChangeLevel changeLevel = ChangeLevel::Full,
                  Utils::SmallStringView defaultPropertyName = {})
        : typeName{typeName}
        , defaultPropertyName{defaultPropertyName}
        , prototype{std::move(prototype)}
        , exportedTypes{std::move(exportedTypes)}
        , propertyDeclarations{std::move(propertyDeclarations)}
        , functionDeclarations{std::move(functionDeclarations)}
        , signalDeclarations{std::move(signalDeclarations)}
        , enumerationDeclarations{std::move(enumerationDeclarations)}
        , accessSemantics{accessSemantics}
        , sourceId{sourceId}
        , changeLevel{changeLevel}
    {}

    explicit Type(Utils::SmallStringView typeName,
                  TypeId prototypeId,
                  TypeAccessSemantics accessSemantics,
                  SourceId sourceId)
        : typeName{typeName}
        , accessSemantics{accessSemantics}
        , sourceId{sourceId}
        , prototypeId{prototypeId}
    {}

    explicit Type(Utils::SmallStringView typeName,
                  ImportedTypeName prototype,
                  TypeAccessSemantics accessSemantics,
                  SourceId sourceId,
                  ChangeLevel changeLevel)
        : typeName{typeName}
        , prototype{std::move(prototype)}
        , accessSemantics{accessSemantics}
        , sourceId{sourceId}
        , changeLevel{changeLevel}
    {}

    explicit Type(Utils::SmallStringView typeName,
                  Utils::SmallStringView prototype,
                  int accessSemantics,
                  int sourceId)
        : typeName{typeName}
        , prototype{ImportedType{prototype}}
        , accessSemantics{static_cast<TypeAccessSemantics>(accessSemantics)}
        , sourceId{sourceId}

    {}

    explicit Type(int sourceId,
                  Utils::SmallStringView typeName,
                  long long typeId,
                  long long prototypeId,
                  int accessSemantics,
                  Utils::SmallStringView defaultPropertyName)
        : typeName{typeName}
        , defaultPropertyName{defaultPropertyName}
        , accessSemantics{static_cast<TypeAccessSemantics>(accessSemantics)}
        , sourceId{sourceId}
        , typeId{typeId}
        , prototypeId{prototypeId}
    {}

    friend bool operator==(const Type &first, const Type &second) noexcept
    {
        return first.typeName == second.typeName
               && first.defaultPropertyName == second.defaultPropertyName
               && first.prototype == second.prototype && first.exportedTypes == second.exportedTypes
               && first.propertyDeclarations == second.propertyDeclarations
               && first.functionDeclarations == second.functionDeclarations
               && first.signalDeclarations == second.signalDeclarations
               && first.sourceId == second.sourceId;
    }

public:
    TypeNameString typeName;
    Utils::SmallString defaultPropertyName;
    ImportedTypeName prototype;
    ExportedTypes exportedTypes;
    PropertyDeclarations propertyDeclarations;
    FunctionDeclarations functionDeclarations;
    SignalDeclarations signalDeclarations;
    EnumerationDeclarations enumerationDeclarations;
    TypeAccessSemantics accessSemantics = TypeAccessSemantics::None;
    SourceId sourceId;
    TypeId typeId;
    TypeId prototypeId;
    ChangeLevel changeLevel = ChangeLevel::Full;
};

using Types = std::vector<Type>;

class ProjectData
{
public:
    ProjectData(SourceId projectSourceId, SourceId sourceId, ModuleId moduleId, FileType fileType)
        : projectSourceId{projectSourceId}
        , sourceId{sourceId}
        , moduleId{moduleId}
        , fileType{fileType}
    {}

    ProjectData(int projectSourceId, int sourceId, int moduleId, int fileType)
        : projectSourceId{projectSourceId}
        , sourceId{sourceId}
        , moduleId{moduleId}
        , fileType{static_cast<FileType>(fileType)}
    {}

    friend bool operator==(const ProjectData &first, const ProjectData &second)
    {
        return first.projectSourceId == second.projectSourceId && first.sourceId == second.sourceId
               && first.moduleId == second.moduleId && first.fileType == second.fileType;
    }

public:
    SourceId projectSourceId;
    SourceId sourceId;
    ModuleId moduleId;
    FileType fileType;
};

using ProjectDatas = std::vector<ProjectData>;

class SynchronizationPackage
{
public:
    SynchronizationPackage() = default;
    SynchronizationPackage(Imports imports, Types types, SourceIds updatedSourceIds)
        : imports{std::move(imports)}
        , types{std::move(types)}
        , updatedSourceIds(std::move(updatedSourceIds))
    {}

    SynchronizationPackage(Imports imports,
                           Types types,
                           SourceIds updatedSourceIds,
                           Imports moduleDependencies,
                           SourceIds updatedModuleDependencySourceIds)
        : imports{std::move(imports)}
        , types{std::move(types)}
        , updatedSourceIds{std::move(updatedSourceIds)}
        , moduleDependencies{std::move(moduleDependencies)}
        , updatedModuleDependencySourceIds{std::move(updatedModuleDependencySourceIds)}
    {}

    SynchronizationPackage(Types types)
        : types{std::move(types)}
    {}

    SynchronizationPackage(SourceIds updatedSourceIds)
        : updatedSourceIds(std::move(updatedSourceIds))
    {}

    SynchronizationPackage(SourceIds updatedFileStatusSourceIds, FileStatuses fileStatuses)
        : updatedFileStatusSourceIds(std::move(updatedFileStatusSourceIds))
        , fileStatuses(std::move(fileStatuses))
    {}

    SynchronizationPackage(SourceIds updatedProjectSourceIds, ProjectDatas projectDatas)
        : projectDatas(std::move(projectDatas))
        , updatedProjectSourceIds(std::move(updatedProjectSourceIds))
    {}

public:
    Imports imports;
    Types types;
    SourceIds updatedSourceIds;
    SourceIds updatedFileStatusSourceIds;
    FileStatuses fileStatuses;
    ProjectDatas projectDatas;
    SourceIds updatedProjectSourceIds;
    Imports moduleDependencies;
    SourceIds updatedModuleDependencySourceIds;
    ModuleExportedImports moduleExportedImports;
    ModuleIds updatedModuleIds;
};

} // namespace QmlDesigner::Storage::Synchronization

namespace QmlDesigner::Storage::Info {

class PropertyDeclaration
{
public:
    PropertyDeclaration(long long typeId,
                        Utils::SmallStringView name,
                        long long traits,
                        long long propertyTypeId)
        : typeId{typeId}
        , name{name}
        , traits{static_cast<PropertyDeclarationTraits>(traits)}
        , propertyTypeId{propertyTypeId}
    {}

    PropertyDeclaration(TypeId typeId,
                        Utils::SmallStringView name,
                        PropertyDeclarationTraits traits,
                        TypeId propertyTypeId)
        : typeId{typeId}
        , name{name}
        , traits{traits}
        , propertyTypeId{propertyTypeId}
    {}

    TypeId typeId;
    Utils::SmallString name;
    PropertyDeclarationTraits traits;
    TypeId propertyTypeId;
};

class Type
{
public:
    Type(long long defaultPropertyId)
        : defaultPropertyId{defaultPropertyId}

    {}

    Type(PropertyDeclarationId defaultPropertyId)
        : defaultPropertyId{defaultPropertyId}
    {}

    PropertyDeclarationId defaultPropertyId;
};

} // namespace QmlDesigner::Storage::Info
