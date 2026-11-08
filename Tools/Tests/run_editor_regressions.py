"""Run inside a disposable UE project containing the plugin and test animations.

Launch with -TestExit="Automation Test Queue Empty" -ReportExportPath=<dir>.
BTL_TEST_FOLDER may select any animation folder copied into the disposable project.
Writes only fixtures under /Game/BTLRegression in a project explicitly marked disposable.
"""
import os,time,unreal
folder=os.environ.get('BTL_TEST_FOLDER','/Game/Props/PrecisionSniperRifle/Character/Animations')
if not os.path.isfile(os.path.join(unreal.Paths.project_dir(),'BTL_DISPOSABLE_TEST_PROJECT')):
    raise RuntimeError('Refusing fixture writes: create BTL_DISPOSABLE_TEST_PROJECT in a disposable project first')
started=time.monotonic()
state={'opened':False,'sized':False,'scrolled':False,'run':False}
tools=unreal.AssetToolsHelpers.get_asset_tools()
material=unreal.load_asset('/Game/BTLRegression/BTL_Time')
if not material:
    material=tools.create_asset('BTL_Time','/Game/BTLRegression',unreal.Material,unreal.MaterialFactoryNew())
    time_node=unreal.MaterialEditingLibrary.create_material_expression(material,unreal.MaterialExpressionTime)
    sine_node=unreal.MaterialEditingLibrary.create_material_expression(material,unreal.MaterialExpressionSine)
    unreal.MaterialEditingLibrary.connect_material_expressions(time_node,'',sine_node,'Input')
    unreal.MaterialEditingLibrary.connect_material_property(sine_node,'',unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    unreal.MaterialEditingLibrary.recompile_material(material)
cascade=unreal.load_asset('/Game/BTLRegression/BTL_Cascade') or tools.create_asset('BTL_Cascade','/Game/BTLRegression',unreal.ParticleSystem,unreal.ParticleSystemFactoryNew())
niagara=unreal.load_asset('/Game/BTLRegression/BTL_Niagara') or unreal.EditorAssetLibrary.duplicate_asset('/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion','/Game/BTLRegression/BTL_Niagara')
for asset in [material,cascade,niagara]:
    if not asset: raise RuntimeError('Rendering fixture creation failed')
    unreal.EditorAssetLibrary.save_loaded_asset(asset,False)

# Enough in-memory animation tiles to force a partially scrolled row even on a large display.
# These duplicates are confined to the marked disposable project and are never saved.
registry=unreal.AssetRegistryHelpers.get_asset_registry()
sequences=[a for a in registry.get_assets_by_path(folder,recursive=False) if str(a.asset_class_path.asset_name)=='AnimSequence']
if not sequences: raise RuntimeError('Copy animation fixtures into BTL_TEST_FOLDER before running')
source=sequences[0].get_asset()
ui_folder='/Game/BTLRegression/TileFixture'
state['ui_assets']=[tools.duplicate_asset('BTL_UI_%03d'%i,ui_folder,source) for i in range(256)]
if not all(state['ui_assets']): raise RuntimeError('UI fixture duplication failed')
started=time.monotonic() # begin UI timing after fixture creation/compilation
def tick(dt):
    age=time.monotonic()-started
    if age>8 and not state['opened']:
        state['opened']=True
        unreal.SystemLibrary.execute_console_command(None,'BongThumbLive.Set Mode VisibleInBrowser')
        unreal.EditorUtilityLibrary.sync_browser_to_folders([ui_folder])
    if age>12 and not state['sized']:
        state['sized']=True
        unreal.SystemLibrary.execute_console_command(None,'BongThumbLive.TestThumbnailSize Huge')
    if age>15 and not state['scrolled']:
        state['scrolled']=True
        unreal.SystemLibrary.execute_console_command(None,'BongThumbLive.ScrollBrowser 0.5')
    if age>20 and not state['run']:
        state['run']=True
        unreal.unregister_slate_post_tick_callback(state['handle'])
        unreal.SystemLibrary.execute_console_command(None,'Automation RunTests BongThumbLive.')
state['handle']=unreal.register_slate_post_tick_callback(tick)
