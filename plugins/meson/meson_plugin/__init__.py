# __init__.py
#
# Copyright (C) 2016 Patrick Griffis <tingping@tingping.se>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

from os import path
import threading
import json
import gi

gi.require_version('Ide', '1.0')

from gi.repository import (
    GLib,
    GObject,
    Gio,
    Ide
)
# TODO: Make *everything* here async!


class MesonBuildSystem(Ide.Object, Ide.BuildSystem, Gio.AsyncInitable):
    project_file = GObject.Property(type=Gio.File)

    def __init__(self, **kwargs):
        super().__init__(**kwargs)

    def do_init_async(self, priority, cancel, callback, data=None):
        task = Gio.Task.new(self, cancel, callback)
        task.set_priority(priority)

        project_file = self.get_context().get_project_file()
        if project_file.get_basename() == 'meson.build':
            task.return_boolean(True)
        else:
            child = project_file.get_child('meson.build')
            exists = child.query_exists(cancel)
            if exists:
                self.project_file = child
            task.return_boolean(exists)

    def do_init_finish(self, result):
        return result.propagate_boolean()

    def do_get_priority(self):
        return -200 # Lower priority than Autotools for now

    def do_get_builder(self, config):
        return MesonBuilder(context=self.get_context(), configuration=config)

    def do_get_build_flags_async(self, file, cancellable, callback):
        task = Gio.Task.new(self, cancellable, callback)
        # TODO
        task.build_flags = []
        task.return_boolean(True)

    def do_get_build_flags_finish(self, result):
        print('Returning flags:', result.build_flags)
        return result.build_flags

    def do_get_build_targets_async(self, cancellable, callback, data=None):
        task = Gio.Task.new(self, cancellable, callback)
        task.build_targets = None

        # FIXME: API cleanup for this?
        config = Ide.Configuration.new(self.get_context(), 'meson-bootstrap', 'local', 'host')
        builder = self.get_builder(config)

        import subprocess
        try:
            ret = subprocess.check_output(['mesonintrospect', '--targets', builder._get_build_dir().get_path()])
        except subprocess.CalledProcessError:
            task.return_error(GLib.Error('Failed to run mesonintrospect'))
            return

        #launcher = Ide.SubprocessLauncher.new(Gio.SubprocessFlags.NONE)
        #launcher.push_args(['mesonintrospect', '--targets', builder._get_build_dir().get_path()])
        #proc = launcher.spawn_sync(cancellable)
        #try:
        #   ret, stdout, stderr = proc.communicate_utf8(None, cancellable)
        #    print(ret, stdout, stderr)
        #    proc.wait(cancellable)
        #except GLib.Error as e:
        #    task.build_targets = None
        #    task.return_error(e)
        #    return

        targets = []
        try:
            meson_targets = json.loads(ret.decode('utf-8'))
        except json.JSONDecodeError:
            task.return_error(GLib.Error('Failed to decode meson json'))
            return

        for t in meson_targets:
            name = t['filename']
            if isinstance(name, list):
                name = name[0]

            install_dir = 'FIXME'
            if t['type'] == 'executable':
                # FIXME: hardcoded, upstream api addition needed
                install_dir = path.join(config.get_prefix(), 'bin')

            ide_target = MesonBuildTarget(install_dir, name=name)
            if t['type'] == 'executable':
                # Sorted by bin first
                targets.insert(0, ide_target)
            else:
                targets.append(ide_target)

        task.build_targets = targets
        task.return_boolean(True)

    def do_get_build_targets_finish(self, result):
        if result.build_targets is None:
            raise result.propagate_error()
        print('Returning targets:', result.build_targets)
        return result.build_targets


class MesonBuilder(Ide.Builder):
    configuration = GObject.Property(type=Ide.Configuration)

    def __init__(self, **kwargs):
        super().__init__(**kwargs)

    def _get_build_dir(self) -> Gio.File:
        context = self.get_context()

        # This matches the Autotools layout
        project_id = context.get_project().get_id()
        buildroot = context.get_root_build_dir()
        device = self.props.configuration.get_device()
        device_id = device.get_id()
        system_type = device.get_system_type()

        return Gio.File.new_for_path(path.join(buildroot, project_id, device_id, system_type))

    def _get_source_dir(self) -> Gio.File:
        context = self.get_context()
        return context.get_vcs().get_working_directory()

    def do_build_async(self, flags, cancellable, callback, data=None):
        task = Gio.Task.new(self, cancellable, callback)
        task.build_result = MesonBuildResult()
        task.build_result.set_mode('Building...')

        builddir = self._get_build_dir()
        # FIXME: 'rebuild' cleans.. not bootstraps?
        clean = flags & Ide.BuilderBuildFlags.FORCE_CLEAN and not flags & Ide.BuilderBuildFlags.FORCE_BOOTSTRAP

        if self.props.configuration.get_dirty() or flags & Ide.BuilderBuildFlags.FORCE_BOOTSTRAP:
            import shutil
            print('deleting builddir')
            try:
                shutil.rmtree(builddir.get_path())
            except FileNotFoundError:
                pass
            self.props.configuration.set_dirty(False)

        if not builddir.query_exists():
            try:
                builddir.make_directory_with_parents(cancellable)
            except GLib.Error as e:
                task.return_error(e)
                return

        if not builddir.get_child('build.ninja').query_exists() and not clean:
            sourcedir = self._get_source_dir()

            launcher = Ide.SubprocessLauncher.new(Gio.SubprocessFlags.NONE)
            launcher.set_cwd(builddir.get_path())

            extra_opts = self.props.configuration.get_config_opts().split()
            extra_opts.append('--prefix=' + self.props.configuration.get_prefix())
            launcher.push_args(['meson', sourcedir.get_path()] + extra_opts)

            subproc = launcher.spawn_sync()
            print("running meson")
            subproc.wait()

        launcher = Ide.SubprocessLauncher.new(Gio.SubprocessFlags.NONE)
        launcher.set_cwd(builddir.get_path())
        launcher.push_args(['ninja'])
        if clean:
            launcher.push_args(['clean'])
        subproc = launcher.spawn_sync()
        try:
            ret = subproc.wait_check()
            task.build_result.set_mode('Building Sucessful')
        except GLib.Error as e:
            task.build_result.set_mode('Building Failed')
            task.return_error(e)
            return

    def do_build_finish(self, result) -> Ide.BuildResult:
        #if result.build_result is None:
        #    raise result.propagate_error() # FIXME: Doesn't work?
        return result.build_result

    def do_install_async(self, cancellable, callback, data=None):
        task = Gio.Task.new(self, cancellable, callback)
        task.build_result = None

        builddir = self._get_build_dir()
        launcher = Ide.SubprocessLauncher.new(Gio.SubprocessFlags.NONE)
        launcher.set_cwd(builddir.get_path())
        launcher.push_args(['ninja', 'install'])
        proc = launcher.spawn_sync()
        proc.wait()

        task.build_result = MesonBuildResult()
        task.return_boolean(True)
        return MesonBuildResult()

    def do_install_finish(self, result) -> Ide.BuildResult:
        if not result.build_result:
            raise result.propagate_error()
        return result.build_result


class MesonBuildResult(Ide.BuildResult):

    def __init__(self, **kwargs):
        super().__init__(**kwargs)


class MesonBuildTarget(Ide.Object, Ide.BuildTarget):
    # FIXME: These should be part of the BuildTarget interface
    name = GObject.Property(type=str)
    install_directory = GObject.Property(type=Gio.File)

    def __init__(self, install_dir, **kwargs):
        super().__init__(**kwargs)
        self.props.install_directory = Gio.File.new_for_path(install_dir)

    def do_get_install_directory(self):
        return self.props.install_directory


#class MesonMiner(Ide.ProjectMiner):
#    def __init__(**kwargs):
#        super().__init__(**kwargs)
#
#    def do_mine_async(self, cancel, callback):
#        pass
#
#    def do_mine_finish(self, result):
#        pass
