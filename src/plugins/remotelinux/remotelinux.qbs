import qbs 1.0

Project {
    name: "RemoteLinux"

    QtcPlugin {
        Depends { name: "Qt.widgets" }
        Depends { name: "QmlDebug" }
        Depends { name: "Utils" }

        Depends { name: "Core" }
        Depends { name: "Debugger" }
        Depends { name: "ProjectExplorer" }

        files: [
            "abstractpackagingstep.cpp",
            "abstractpackagingstep.h",
            "abstractremotelinuxdeployservice.cpp",
            "abstractremotelinuxdeployservice.h",
            "abstractremotelinuxdeploystep.cpp",
            "abstractremotelinuxdeploystep.h",
            "deploymenttimeinfo.cpp",
            "deploymenttimeinfo.h",
            "genericdirectuploadservice.cpp",
            "genericdirectuploadservice.h",
            "genericdirectuploadstep.cpp",
            "genericdirectuploadstep.h",
            "genericlinuxdeviceconfigurationwidget.cpp",
            "genericlinuxdeviceconfigurationwidget.h",
            "genericlinuxdeviceconfigurationwidget.ui",
            "genericlinuxdeviceconfigurationwizard.cpp",
            "genericlinuxdeviceconfigurationwizard.h",
            "genericlinuxdeviceconfigurationwizardpages.cpp",
            "genericlinuxdeviceconfigurationwizardpages.h",
            "genericlinuxdeviceconfigurationwizardsetuppage.ui",
            "killappstep.cpp",
            "killappstep.h",
            "linuxdevice.cpp",
            "linuxdevice.h",
            "linuxdevicetester.cpp",
            "linuxdevicetester.h",
            "linuxprocessinterface.h",
            "makeinstallstep.cpp",
            "makeinstallstep.h",
            "publickeydeploymentdialog.cpp",
            "publickeydeploymentdialog.h",
            "remotelinux.qrc",
            "remotelinux_constants.h",
            "remotelinux_export.h",
            "remotelinuxcheckforfreediskspacestep.cpp",
            "remotelinuxcheckforfreediskspacestep.h",
            "remotelinuxcustomcommanddeploymentstep.cpp",
            "remotelinuxcustomcommanddeploymentstep.h",
            "remotelinuxcustomrunconfiguration.cpp",
            "remotelinuxcustomrunconfiguration.h",
            "remotelinuxdebugsupport.cpp",
            "remotelinuxdebugsupport.h",
            "remotelinuxdeployconfiguration.cpp",
            "remotelinuxdeployconfiguration.h",
            "remotelinuxenvironmentaspect.cpp",
            "remotelinuxenvironmentaspect.h",
            "remotelinuxenvironmentaspectwidget.cpp",
            "remotelinuxenvironmentaspectwidget.h",
            "remotelinuxenvironmentreader.cpp",
            "remotelinuxenvironmentreader.h",
            "remotelinuxpackageinstaller.cpp",
            "remotelinuxpackageinstaller.h",
            "remotelinuxplugin.cpp",
            "remotelinuxplugin.h",
            "remotelinuxqmltoolingsupport.cpp",
            "remotelinuxqmltoolingsupport.h",
            "remotelinuxrunconfiguration.cpp",
            "remotelinuxrunconfiguration.h",
            "remotelinuxsignaloperation.cpp",
            "remotelinuxsignaloperation.h",
            "remotelinuxx11forwardingaspect.cpp",
            "remotelinuxx11forwardingaspect.h",
            "rsyncdeploystep.cpp",
            "rsyncdeploystep.h",
            "sshkeycreationdialog.cpp",
            "sshkeycreationdialog.h",
            "sshkeycreationdialog.ui",
            "sshprocessinterface.h",
            "tarpackagecreationstep.cpp",
            "tarpackagecreationstep.h",
            "tarpackagedeploystep.cpp",
            "tarpackagedeploystep.h",
            "images/embeddedtarget.png",
        ]

        Group {
            name: "Tests"
            condition: qtc.testsEnabled
            files: [
                "filesystemaccess_test.cpp",
                "filesystemaccess_test.h",
            ]
        }

        Export {
            Depends { name: "Debugger" }
            Depends { name: "Core" }
        }
    }
}
