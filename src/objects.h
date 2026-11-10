// Implementation objects. COM interface wrappers (one per interface version)
// live in wrappers.h and forward to these.
#pragma once
#include "common.h"
#include "gpu.h"
#include "overlay.h"
#include "presenter.h"

#include "surfaceobject.h"

// The DirectDraw object (after the Presenter its inline functions call).
#include "ddobject.h"
