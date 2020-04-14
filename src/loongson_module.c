#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <fcntl.h>
#include "xf86.h"
#include "xf86_OSproc.h"
#include "compiler.h"
#include "xf86Pci.h"
#include "mipointer.h"
#include "micmap.h"
#include <X11/extensions/randr.h>
#include "fb.h"
#include "edid.h"
#include "xf86i2c.h"
#include "xf86Crtc.h"
#include "miscstruct.h"
#include "dixstruct.h"
#include "shadow.h"
#include "xf86xv.h"
#include <X11/extensions/Xv.h>
#include <xorg-server.h>
#ifdef XSERVER_PLATFORM_BUS
#include "xf86platformBus.h"
#endif
#if XSERVER_LIBPCIACCESS
#include <pciaccess.h>
#endif

#include "compat-api.h"
#include "driver.h"


static Bool Probe(DriverPtr drv, int flags);
static void Identify(int flags);
static Bool ms_pci_probe(DriverPtr driver, 
				int entity_num, struct pci_device *device, intptr_t match_data);

static Bool ms_driver_func(ScrnInfoPtr scrn, xorgDriverFuncOp op, void *data);

#ifdef XSERVER_PLATFORM_BUS
static Bool ms_platform_probe(DriverPtr driver, int entity_num, int flags, 
				struct xf86_platform_device *device, intptr_t match_data);
#endif


static SymTabRec Chipsets[] =
{
	{0, "LOONGSON 7A1000"},
	{1, "LOONGSON 7A2000"},
	{2, "LOONGSON 2K"},
	{3, "LOONGSON 2H"},
	{-1, NULL}
};

static void Identify(int flags)
{
    xf86PrintChipsets("loongson", "Driver for Modesetting Kernel Drivers", Chipsets);
}


static int check_outputs(int fd)
{
    drmModeResPtr res = drmModeGetResources(fd);
    int ret;

    if (!res)
        return FALSE;
    ret = res->count_connectors > 0;
    drmModeFreeResources(res);
    return ret;
}


static Bool probe_hw(const char *dev, struct xf86_platform_device *platform_dev)
{
    int fd;

#if XF86_PDEV_SERVER_FD
    if (platform_dev && (platform_dev->flags & XF86_PDEV_SERVER_FD))
    {
        fd = xf86_get_platform_device_int_attrib(platform_dev, ODEV_ATTRIB_FD, -1);
        if (fd == -1)
            return FALSE;
        return check_outputs(fd);
    }
#endif

    fd = LS_OpenHW(dev);
    if (fd != -1)
    {
        int ret = check_outputs(fd);
        close(fd);
        return ret;
    }
    return FALSE;
}

static Bool Probe(DriverPtr drv, int flags)
{
    int i, numDevSections;
    GDevPtr *devSections;
    Bool foundScreen = FALSE;
    const char *dev;
    ScrnInfoPtr scrn = NULL;

    /* For now, just bail out for PROBE_DETECT. */
    if (flags & PROBE_DETECT) {
        return FALSE;
    }
    /*
     * Find the config file Device sections that match this
     * driver, and return if there are none.
     */
    if ((numDevSections = xf86MatchDevice("loongson", &devSections)) <= 0)
    {
        return FALSE;
    }

    for (i = 0; i < numDevSections; ++i)
    {

        dev = xf86FindOptionValue(devSections[i]->options, "kmsdev");

        if (probe_hw(dev, NULL)) {
            int entity;
            entity = xf86ClaimFbSlot(drv, 0, devSections[i], TRUE);
            scrn = xf86ConfigFbEntity(scrn, 0, entity, NULL, NULL, NULL, NULL);
        }

        if (scrn)
        {
            LS_HookUpScreen( scrn, Probe );

            xf86DrvMsg(scrn->scrnIndex, X_INFO, 
                "using %s\n", dev ? dev : "default device");
        }
    }

    free(devSections);

    return foundScreen;
}


static Bool ms_driver_func(ScrnInfoPtr scrn, xorgDriverFuncOp op, void *data)
{
    xorgHWFlags *flag;
    
    switch (op)
    {
        case GET_REQUIRED_HW_INTERFACES:
        flag = (CARD32 *)data;
        (*flag) = 0;
        return TRUE;
#if XORG_VERSION_CURRENT >= XORG_VERSION_NUMERIC(1,15,99,902,0)
        case SUPPORTS_SERVER_FDS:
            return TRUE;
#endif
        default:
            return FALSE;
    }
}


#ifdef XSERVER_LIBPCIACCESS
static const struct pci_id_match ms_device_match[] = {
    {
        PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY, PCI_MATCH_ANY,
        0x00030000, 0x00ff0000, 0
    },

    { 0, 0, 0 },
};
#endif


#if XSERVER_LIBPCIACCESS

static char * ms_DRICreatePCIBusID(const struct pci_device *dev)
{
    char *busID;

    if (asprintf(&busID, "pci:%04x:%02x:%02x.%d",
                 dev->domain, dev->bus, dev->dev, dev->func) == -1)
        return NULL;

    return busID;
}



static Bool probe_hw_pci(const char *dev, struct pci_device *pdev)
{
    int ret = FALSE;
    int fd = LS_OpenHW(dev);

    char *id, *devid;
    drmSetVersion sv;

    if (fd == -1) 
    {
        return FALSE;
    }

    sv.drm_di_major = 1;
    sv.drm_di_minor = 4;
    sv.drm_dd_major = -1;
    sv.drm_dd_minor = -1;
    if (drmSetInterfaceVersion(fd, &sv)) {
        close(fd);
        return FALSE;
    }

    id = drmGetBusid(fd);
    devid = ms_DRICreatePCIBusID(pdev);

    if (id && devid && !strcmp(id, devid)) 
    {
        ret = check_outputs(fd);
    }

    close(fd);
    free(id);
    free(devid);
    return ret;
}


static Bool ms_pci_probe(DriverPtr driver,
               int entity_num, struct pci_device *dev, intptr_t match_data)
{
    ScrnInfoPtr scrn = NULL;

    scrn = xf86ConfigPciEntity(scrn, 0, entity_num, NULL,
			       NULL, NULL, NULL, NULL, NULL);
    if (scrn)
    {
        const char *devpath;
        GDevPtr devSection = xf86GetDevFromEntity(scrn->entityList[0],
						  scrn->entityInstanceList[0]);

	devpath = xf86FindOptionValue(devSection->options, "kmsdev");
	if (probe_hw_pci(devpath, dev))
	{
		LS_HookUpScreen(scrn, NULL);

	    xf86DrvMsg(scrn->scrnIndex, X_CONFIG,
		       "claimed PCI slot %d@%d:%d:%d\n", 
		       dev->bus, dev->domain, dev->dev, dev->func);
	    xf86DrvMsg(scrn->scrnIndex, X_INFO,
		       "using %s\n", devpath ? devpath : "default device");
	} else
	    scrn = NULL;
    }
    return scrn != NULL;
}
#endif


#ifdef XSERVER_PLATFORM_BUS
static Bool ms_platform_probe(DriverPtr driver,
    int entity_num, int flags, struct xf86_platform_device *dev, intptr_t match_data)
{
    ScrnInfoPtr scrn = NULL;
    const char *path = xf86_get_platform_device_attrib(dev, ODEV_ATTRIB_PATH);
    int scr_flags = 0;

    if (flags & PLATFORM_PROBE_GPU_SCREEN)
            scr_flags = XF86_ALLOCATE_GPU_SCREEN;

    if (probe_hw(path, dev))
    {
        scrn = xf86AllocateScreen(driver, scr_flags);
        xf86AddEntityToScreen(scrn, entity_num);

		LS_HookUpScreen(scrn, NULL);


        xf86DrvMsg(scrn->scrnIndex, X_INFO,
                   "using drv %s\n", path ? path : "default device");
    }

    return scrn != NULL;
}
#endif



static MODULESETUPPROTO(Setup);

static XF86ModuleVersionInfo VersRec = {
    "loongson",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_VIDEODRV,
    ABI_VIDEODRV_VERSION,
    MOD_CLASS_VIDEODRV,
    {0, 0, 0, 0}
};


_X_EXPORT DriverRec I_LoongsonDrv = {
    1,
    "loongson",
    Identify,
    Probe,
    LS_AvailableOptions,
    NULL,
    0,
    ms_driver_func,
    ms_device_match,
    ms_pci_probe,
#ifdef XSERVER_PLATFORM_BUS
    ms_platform_probe,
#endif
};


static void * Setup(void * module, void * opts, int *errmaj, int *errmin)
{
    static Bool setupDone = 0;

    // This module should be loaded only once, but check to be sure.
    if (!setupDone)
    {
        setupDone = 1;
        xf86AddDriver(&I_LoongsonDrv, module, HaveDriverFuncs);

     /*
      * The return value must be non-NULL on success even though there
      * is no TearDownProc.
      */
        return (void *) 1;
    }
    else
    {
        if (errmaj) {
            *errmaj = LDR_ONCEONLY;
        }
        return NULL;
    }
}


_X_EXPORT XF86ModuleData loongsonModuleData = { &VersRec, Setup, NULL };
