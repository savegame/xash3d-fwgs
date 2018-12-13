# encoding: utf-8
# sdl2.py -- sdl2 waf plugin
# Copyright (C) 2018 a1batross
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

import os

def options(opt):
	opt.add_option(
		'--sdl2', action='store', type='string', dest = 'SDL2_PATH', default = None,
		help = 'SDL2 path to build(required for Windows)')

	opt.add_option(
		'--skip-sdl2-sanity-check', action='store_false', default = True, dest='SDL2_SANITY_CHECK',
		help = 'Skip checking SDL2 sanity')

def configure(conf):
	if conf.options.SDL2_PATH:
		conf.start_msg('Configuring SDL2 by provided path')
		conf.env.HAVE_SDL2 = 1
		conf.env.INCLUDES_SDL2 = [
			os.path.abspath(os.path.join(conf.options.SDL2_PATH, 'include')),
			os.path.abspath(os.path.join(conf.options.SDL2_PATH, 'include/SDL2'))
		]
		libpath = 'lib'
		if conf.env.COMPILER_CC == 'msvc':
			if conf.env.DEST_CPU == 'x86_64':
				libpath = 'lib/x64'
			else:
				libpath = 'lib/' + conf.env.DEST_CPU
		conf.env.LIBPATH_SDL2 = [os.path.abspath(os.path.join(conf.options.SDL2_PATH, libpath))]
		conf.env.LIB_SDL2 = ['SDL2']
		conf.end_msg('yes: {0}, {1}, {2}'.format(conf.env.LIB_SDL2, conf.env.LIBPATH_SDL2, conf.env.INCLUDES_SDL2))
	else:
		try:
			conf.check_cfg(
				path='sdl2-config',
				args='--cflags --libs',
				package='',
				msg='Checking for library SDL2',
				uselib_store='SDL2')
		except conf.errors.ConfigurationError:
			conf.env.HAVE_SDL2 = 0

	if conf.env.HAVE_SDL2 and conf.options.SDL2_SANITY_CHECK:
		try:
			conf.check_cc(
				fragment='''
				#define SDL_MAIN_HANDLED
				#include <SDL.h>
				int main( void )
				{
					SDL_Init( SDL_INIT_EVERYTHING );
					return 0;
				}''',
				msg	= 'Checking for library SDL2 sanity',
				use = 'SDL2',
				execute = False)
		except conf.errors.ConfigurationError:
			conf.env.HAVE_SDL2 = 0
