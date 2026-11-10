import os.path
import zipfile

top = '.'
out = 'build'

def build_phone(task):
    # Clay ships its compiled phone code inside a Pebble package archive.
    # Use the published artifact instead of its unbuilt development entry point.
    project = task.generator.bld.path
    clay = project.get_bld().make_node('clay.js')
    with zipfile.ZipFile(project.find_node('node_modules/@rebble/clay/dist.zip').abspath()) as archive:
        clay.write(archive.read('js/index.js').decode('utf-8'))
    return task.exec_command([
        'node', project.find_node('scripts/build-phone.mjs').abspath(),
        project.get_bld().make_node('js/message_keys.json').abspath(),
        clay.abspath(), task.outputs[0].abspath()
    ], cwd=project.abspath())

def options(ctx):
    ctx.load('pebble_sdk')

def configure(ctx):
    ctx.load('pebble_sdk')

def build(ctx):
    ctx.load('pebble_sdk')
    binaries = []
    cached_env = ctx.env
    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.env = ctx.all_envs[platform]
        ctx.set_group(ctx.env.PLATFORM_NAME)
        app_elf = '{}/pebble-app.elf'.format(ctx.env.BUILD_DIR)
        ctx.pbl_build(source=ctx.path.ant_glob('src/c/**/*.c'), target=app_elf, bin_type='app')
        binaries.append({'platform': platform, 'app_elf': app_elf})
    ctx.env = cached_env
    ctx.set_group('bundle')
    phone_js = ctx.path.get_bld().make_node('pebble-js-app.js')
    phone_map = ctx.path.get_bld().make_node('pebble-js-app.js.map')
    sources = ctx.path.ant_glob(['src/pkjs/**/*.js', 'src/pkjs/**/*.mjs', 'src/pkjs/**/*.json'])
    sources += [ctx.path.find_node('scripts/build-phone.mjs'), ctx.path.find_node('package-lock.json'),
                ctx.path.find_node('node_modules/@rebble/clay/dist.zip'),
                ctx.path.get_bld().make_node('js/message_keys.json')]
    ctx(rule=build_phone, source=sources, target=[phone_js, phone_map], name='phone_companion')
    ctx.pbl_bundle(binaries=binaries, js=[phone_js, phone_map])
