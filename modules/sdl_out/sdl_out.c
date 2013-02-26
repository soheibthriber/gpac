/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre 
 *			Copyright (c) Telecom ParisTech 2000-2012
 *					All rights reserved
 *
 *  This file is part of GPAC / SDL audio and video module
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *   
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *		
 */

#include "sdl_out.h"

#if !defined(__GNUC__)
# if defined(_WIN32_WCE)
#  pragma comment(lib, "SDL")
# elif defined (WIN32)
#  pragma comment(lib, "SDL")
# endif
#endif

static Bool is_init = GF_FALSE;
static u32 num_users = 0;

Bool SDLOUT_InitSDL()
{
	if (is_init) {
		num_users++;
		return GF_TRUE;
	}
	if (SDL_Init(0) < 0) return GF_FALSE;
	is_init = GF_TRUE;
	num_users++;
	return GF_TRUE;
}

void SDLOUT_CloseSDL()
{
	if (!is_init) return;
	assert(num_users);
	num_users--;
	if (!num_users) SDL_Quit();
	return;
}


/*interface query*/
GPAC_MODULE_EXPORT
const u32 *QueryInterfaces() 
{
	static u32 si [] = {
		GF_VIDEO_OUTPUT_INTERFACE,
		GF_AUDIO_OUTPUT_INTERFACE,
		0
	};
	return si; 
}

/*interface create*/
GPAC_MODULE_EXPORT
GF_BaseInterface *LoadInterface(u32 InterfaceType)
{
	if (InterfaceType == GF_VIDEO_OUTPUT_INTERFACE) return SDL_NewVideo();
	if (InterfaceType == GF_AUDIO_OUTPUT_INTERFACE) return SDL_NewAudio();
	return NULL;
}

/*interface destroy*/
GPAC_MODULE_EXPORT
void ShutdownInterface(GF_BaseInterface *ifce)
{
	switch (ifce->InterfaceType) {
	case GF_VIDEO_OUTPUT_INTERFACE:
		SDL_DeleteVideo(ifce);
		break;
	case GF_AUDIO_OUTPUT_INTERFACE:
		SDL_DeleteAudio(ifce);
		break;
	}
}

GPAC_MODULE_STATIC_DELARATION( sdl_out )
