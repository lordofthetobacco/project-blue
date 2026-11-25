"""Bakes "Robot Kyle" posed holding the sample weapons into static meshes.

Run headless (Blender 5.3):

  blender -b --python tools/assets/kyle/bake_kyle_poses.py
      [-- <Robot Kyle.fbx> [<pose name> ...]]

(default FBX: ~/Downloads/Robot Kyle.fbx; default: every pose).

Reads the rigged FBX (only reads it; nothing is saved as .blend), imports the
weapon sources the engine cooks (samples/m16.obj, samples/glock/glock17.glb)
as posing references at the engine's scale, poses the armature per entry of
POSES below and writes, per pose, into samples/kyle/:

  <pose>.glb   the posed figure as one static mesh (normals, UVs, one
               material "kyle"), Y-up, metres, facing -Z, origin = the centre
               between the feet on the ground;
  <pose>.pose  "key = value" text: the held weapon prefab and its transform in
               figure space, and the hand frames (see WritePose()).

Preview renders (Workbench: front, side, top, three-quarter, both hands,
two close-ups per hand) go to out/tools/logs/kyle/<pose>_<view>.png. The
wrist IK error and the arm reach per hand are printed.

Coordinates: everything in the tables is in the engine's figure space
(metres, +Y up, the figure faces -Z, its right is +X) or in a weapon's prefab
space (metres; the model's axes times the cook's scale). Blender works Z-up
with the figure facing +Y after the 180 degree turn applied on import;
ENGINE_TO_BLENDER converts.

Arms are posed with two-bone IK onto wrist targets fixed to the weapon, the
hands are turned onto a frame given by finger direction and palm normal, and
everything else (spine, head, clavicles, fingers, thumbs) comes from the
per-bone rotation tables (degrees about named axes).
"""

import math
import os
import sys

import bpy
from mathutils import Matrix, Quaternion, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
SAMPLES = os.path.join(REPO, "samples")
OUT_DIR = os.path.join(SAMPLES, "kyle")
PREVIEW_DIR = os.path.join(REPO, "out", "tools", "logs", "kyle")
DEFAULT_FBX = os.path.join(os.path.expanduser("~"), "Downloads",
                           "Robot Kyle.fbx")

# Engine (x, y, z) -> Blender (x, -z, y); the OBJ and glTF importers and the
# glTF exporter use the same convention.
ENGINE_TO_BLENDER = Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0),
                            (0, 0, 0, 1)))
BLENDER_TO_ENGINE = ENGINE_TO_BLENDER.inverted()

# The weapons as blue-pack samples cooks them (tools/blue-pack/samples.cc,
# weapon::M16Geometry() / Glock17Geometry()).
WEAPONS = {
    "m16": {
        "source": os.path.join(SAMPLES, "m16.obj"),
        "scale": 0.0771,
        "prefab": "m16.pbo",
        "drop": [],
    },
    "glock17": {
        "source": os.path.join(SAMPLES, "glock", "glock17.glb"),
        "scale": 0.0842,
        "prefab": "glock17.pbo",
        "drop": ["Glock_parts_ORIGINAL_combined"],
    },
}

FIGURE_MATERIAL = (0.62, 0.64, 0.66, 1.0)  # preview only; kyle.bmat rules


def V(*xyz):
  return Vector(xyz)


# Each pose:
#   weapon        key of WEAPONS;
#   weapon_turn   (axis, degrees) rotations applied in order to the weapon's
#                 prefab space, axes of figure space;
#   body_turn     degrees about +Y the whole body (bone Root) is turned
#                 while the weapon keeps pointing down range (a bladed
#                 stance); anchor_body and the poles turn with it;
#   anchor_model  a point of the weapon (prefab space, metres) ...
#   anchor_body   ... and where it goes, body space (figure space before
#                 body_turn);
#   hands         per side: "grip", the palm centre (prefab space of the
#                 weapon); the flat hand's finger direction "fingers" and
#                 palm normal "palm" (prefab space of the weapon); the IK
#                 pole for the elbow (body space);
#   bones         {bone: [(axis, degrees), ...]}: axes "x", "y", "z" of
#                 figure space, or "curl" / "spread" / "twist" of the bone's
#                 own hand at rest (curl bends a finger towards the palm).
M16_SCALE = WEAPONS["m16"]["scale"]
GLOCK_SCALE = WEAPONS["glock17"]["scale"]

# The palm centre sits this far from the midpoint between the wrist joint
# and the middle finger's knuckle, towards the palm (m).
PALM_DEPTH = 0.015


def M16(x, y, z):
  return Vector((x, y, z)) * M16_SCALE


def G17(x, y, z):
  return Vector((x, y, z)) * GLOCK_SCALE


FINGERS_GRIP = {
    # Wrapped round a grip: about 90 degrees over the three joints.
    "Index": (40, 55, 30),
    "Middle": (45, 60, 30),
    "Ring": (45, 60, 30),
    "Pinky": (45, 60, 30),
}


def FingerTable(side, curls, thumb):
  table = {}
  for finger, (a, b, c) in curls.items():
    base = f"{side}_{finger}_Finger_Joint_01"
    table[base + "a"] = [("curl", a)]
    table[base + "b"] = [("curl", b)]
    table[base + "c"] = [("curl", c)]
  table[f"{side}_Thumb_Joint_01a"] = thumb[0]
  table[f"{side}_Thumb_Joint_01b"] = thumb[1]
  return table


POSES = {
    "kyle_rifle": {
        "weapon": "m16",
        # Muzzle down range (-Z), then muzzle-down: low ready with the stock
        # in the shoulder.
        "weapon_turn": [("y", 180.0), ("x", -30.0)],
        "body_turn": -50.0,
        "anchor_model": M16(0.0, -0.1, -5.85),  # butt plate centre
        "anchor_body": V(0.13, 1.30, -0.11),  # right shoulder pocket
        "hands": {
            "Right": {
                "grip": M16(-0.5, -0.65, -2.1),
                "fingers": V(0.0, -0.66, 0.75),
                "palm": V(1.0, 0.0, 0.0),
                "pole": V(0.5, 0.7, 0.3),
            },
            "Left": {
                "grip": M16(0.1, 0.05, 1.2),
                "fingers": V(-1.0, 0.0, 0.0),
                "palm": V(0.0, 1.0, 0.0),
                "pole": V(-0.4, 0.6, 0.0),
            },
        },
        "bones": {
            "Ribs": [("x", -6.0)],
            "Neck": [("x", -4.0)],
            "Head": [("x", -6.0), ("y", 40.0)],
            "Left_Shoulder_Joint_01": [("y", 20.0)],
            **FingerTable("Right", FINGERS_GRIP,
                          ([("curl", 25.0)], [("curl", 25.0)])),
            **FingerTable("Left", FINGERS_GRIP,
                          ([("curl", 10.0)], [("curl", 10.0)])),
        },
    },
    "kyle_pistol": {
        "weapon": "glock17",
        # Bore (-X) down range (-Z), level.
        "weapon_turn": [("y", -90.0)],
        "body_turn": 0.0,
        "anchor_model": G17(0.78, -0.35, 0.0),  # grip
        "anchor_body": V(0.02, 1.40, -0.42),
        "hands": {
            "Right": {
                "grip": G17(0.85, -0.3, -0.35),
                "fingers": V(-1.0, -0.4, 0.0),
                "palm": V(0.0, 0.0, 1.0),
                "pole": V(0.6, 0.6, 0.2),
            },
            "Left": {
                "grip": G17(0.65, -0.4, 0.4),
                "fingers": V(-0.8, -0.8, 0.0),
                "palm": V(0.0, 0.0, -1.0),
                "pole": V(-0.6, 0.6, 0.2),
            },
        },
        "bones": {
            "Head": [("x", -3.0)],
            **FingerTable("Right", FINGERS_GRIP,
                          ([("curl", 20.0)], [("curl", 20.0)])),
            **FingerTable("Left", FINGERS_GRIP,
                          ([("curl", 10.0)], [("curl", 10.0)])),
        },
    },
}

VIEWS = {
    # name: (camera position, look-at point), figure space.
    "front": (V(0.0, 1.1, -4.2), V(0.0, 0.95, 0.0)),
    "side": (V(4.2, 1.1, 0.0), V(0.0, 0.95, -0.2)),
    "top": (V(0.0, 5.0, 0.2), V(0.0, 0.0, -0.25)),
    "three_quarter": (V(2.8, 1.8, -3.0), V(0.0, 0.95, -0.2)),
    "hands": (V(1.0, 1.6, -1.3), V(0.05, 1.2, -0.35)),
}


def ToBlender(point):
  return (ENGINE_TO_BLENDER @ Vector((*point, 1.0))).xyz


def ToEngine(point):
  return (BLENDER_TO_ENGINE @ Vector((*point, 1.0))).xyz


def TurnMatrix(turns):
  rotation = Quaternion()
  axes = {"x": V(1, 0, 0), "y": V(0, 1, 0), "z": V(0, 0, 1)}
  for axis, degrees in turns:
    rotation = Quaternion(axes[axis], math.radians(degrees)) @ rotation
  return rotation


def ResetScene():
  bpy.ops.wm.read_factory_settings(use_empty=True)


def ImportKyle(fbx):
  bpy.ops.import_scene.fbx(filepath=fbx, automatic_bone_orientation=True)
  armature = next(o for o in bpy.data.objects if o.type == "ARMATURE")
  mesh = next(o for o in bpy.data.objects if o.type == "MESH")
  # Point each arm bone's tail at its child's head so two-bone IK on the
  # forearm puts the wrist on the target. Changing rest bones does not move
  # the skin (it deforms by pose relative to rest).
  bpy.context.view_layer.objects.active = armature
  bpy.ops.object.mode_set(mode="EDIT")
  edit = armature.data.edit_bones
  for side in ("Left", "Right"):
    chain = [f"{side}_Upper_Arm_Joint_01", f"{side}_Forearm_Joint_01",
             f"{side}_Wrist_Joint_01"]
    for parent, child in zip(chain, chain[1:]):
      edit[parent].tail = edit[child].head
  bpy.ops.object.mode_set(mode="OBJECT")
  # Face +Y in Blender = -Z in the engine.
  armature.matrix_world = (Matrix.Rotation(math.pi, 4, "Z") @
                           armature.matrix_world)
  bpy.context.view_layer.update()
  return armature, mesh


def ImportWeapon(key):
  weapon = WEAPONS[key]
  before = set(bpy.data.objects)
  if weapon["source"].endswith(".obj"):
    bpy.ops.wm.obj_import(filepath=weapon["source"])
  else:
    bpy.ops.import_scene.gltf(filepath=weapon["source"])
  added = [o for o in bpy.data.objects if o not in before]
  for name in weapon["drop"]:
    for o in added:
      if o.name == name:
        bpy.data.objects.remove(o)
  added = [o for o in bpy.data.objects if o not in before]
  holder = bpy.data.objects.new(key + "_holder", None)
  bpy.context.scene.collection.objects.link(holder)
  for o in added:
    if o.parent is None:
      world = o.matrix_world.copy()
      o.parent = holder
      o.matrix_world = world
  return holder


def BodyTurn(pose):
  return TurnMatrix([("y", pose["body_turn"])])


def WeaponMatrix(pose):
  """Returns the weapon prefab space -> figure space transform (engine)."""
  rotation = TurnMatrix(pose["weapon_turn"])
  anchor = BodyTurn(pose) @ pose["anchor_body"]
  translation = anchor - rotation @ pose["anchor_model"]
  return Matrix.Translation(translation) @ rotation.to_matrix().to_4x4()


def RestHandFrame(armature, side):
  """Returns finger direction, palm normal and across (index -> pinky) of a
  hand at rest, in armature space."""
  bones = armature.data.bones
  wrist = bones[f"{side}_Wrist_Joint_01"].head_local
  middle = bones[f"{side}_Middle_Finger_Joint_01a"].head_local
  index = bones[f"{side}_Index_Finger_Joint_01a"].head_local
  pinky = bones[f"{side}_Pinky_Finger_Joint_01a"].head_local
  thumb = bones[f"{side}_Thumb_Joint_01b"].head_local
  fingers = (middle - wrist).normalized()
  across = (pinky - index)
  across = (across - fingers * across.dot(fingers)).normalized()
  palm = fingers.cross(across).normalized()
  if palm.dot(thumb - (wrist + middle) * 0.5) < 0.0:
    palm = -palm
  return fingers, palm, across


def FrameMatrix(fingers, palm):
  fingers = fingers.normalized()
  palm = (palm - fingers * palm.dot(fingers)).normalized()
  return Matrix((fingers, palm, fingers.cross(palm))).transposed()


def AddEmpty(name, matrix):
  empty = bpy.data.objects.new(name, None)
  bpy.context.scene.collection.objects.link(empty)
  empty.matrix_world = matrix
  return empty


def RestPalmOffset(armature, side):
  """Returns the vector from the wrist joint to the palm centre at rest,
  Blender world space."""
  bones = armature.data.bones
  world = armature.matrix_world
  wrist = world @ bones[f"{side}_Wrist_Joint_01"].head_local
  middle = world @ bones[f"{side}_Middle_Finger_Joint_01a"].head_local
  _, palm, _ = RestHandFrame(armature, side)
  palm = (world.to_3x3() @ palm).normalized()
  return (middle - wrist) * 0.5 + palm * PALM_DEPTH


def PoseArm(armature, side, hand, weapon_matrix, body_turn):
  """IK on the forearm chain to the wrist target, and the hand turned onto
  the target frame with Copy Rotation. All targets are Blender empties.
  Returns what PickPoleAngle() needs and the wrist target (engine)."""
  to_blender = ENGINE_TO_BLENDER.to_3x3()
  arm_world = armature.matrix_world
  rotation3 = weapon_matrix.to_3x3()
  grip_engine = (weapon_matrix @ hand["grip"].to_4d()).xyz
  fingers_engine = rotation3 @ hand["fingers"]
  palm_engine = rotation3 @ hand["palm"]

  # Hand frame: rest hand frame (Blender world) -> target frame.
  rest_fingers, rest_palm, _ = RestHandFrame(armature, side)
  arm3 = arm_world.to_3x3()
  rest_frame = FrameMatrix(arm3 @ rest_fingers, arm3 @ rest_palm)
  target_frame = FrameMatrix(to_blender @ fingers_engine,
                             to_blender @ palm_engine)
  turn = target_frame @ rest_frame.inverted()
  wrist_bone = armature.data.bones[f"{side}_Wrist_Joint_01"]
  wrist_rest = (arm_world @ wrist_bone.matrix_local).to_3x3().normalized()
  hand_frame = AddEmpty(f"{side}_hand_frame", (turn @ wrist_rest).to_4x4())
  copy = armature.pose.bones[f"{side}_Wrist_Joint_01"].constraints.new(
      "COPY_ROTATION")
  copy.target = hand_frame

  # The wrist goes where it puts the palm centre on the grip point.
  wrist_blender = (ToBlender(grip_engine) -
                   turn @ RestPalmOffset(armature, side))
  target = AddEmpty(f"{side}_wrist_target",
                    Matrix.Translation(wrist_blender))
  pole = AddEmpty(f"{side}_pole",
                  Matrix.Translation(ToBlender(body_turn @ hand["pole"])))
  forearm = armature.pose.bones[f"{side}_Forearm_Joint_01"]
  ik = forearm.constraints.new("IK")
  ik.target = target
  ik.pole_target = pole
  ik.chain_count = 2
  ik.use_stretch = False
  return forearm, ik, pole, ToEngine(wrist_blender)


def PickPoleAngle(armature, forearm, ik, pole):
  """Tries the four quarter turns and keeps the one that bends the elbow
  closest to the pole target."""
  best = None
  for degrees in (-180.0, -90.0, 0.0, 90.0):
    ik.pole_angle = math.radians(degrees)
    bpy.context.view_layer.update()
    elbow = armature.matrix_world @ forearm.head
    distance = (elbow - pole.matrix_world.translation).length
    if best is None or distance < best[0]:
      best = (distance, degrees)
  ik.pole_angle = math.radians(best[1])
  bpy.context.view_layer.update()


def BoneAxes(armature, bone_name):
  """Named rotation axes of a bone in its rest (local) space."""
  bone = armature.data.bones[bone_name]
  to_local = bone.matrix_local.to_3x3().inverted()
  axes = {}
  # Figure axes: engine -> Blender world -> armature space.
  world_to_armature = armature.matrix_world.to_3x3().inverted()
  for name, engine_axis in (("x", V(1, 0, 0)), ("y", V(0, 1, 0)),
                            ("z", V(0, 0, 1))):
    armature_axis = world_to_armature @ (ENGINE_TO_BLENDER.to_3x3() @
                                         engine_axis)
    axes[name] = (to_local @ armature_axis).normalized()
  for side in ("Left", "Right"):
    if bone_name.startswith(side) and ("Finger" in bone_name or
                                       "Thumb" in bone_name):
      fingers, palm, across = RestHandFrame(armature, side)
      # Curl: about the across axis, fingertips towards the palm.
      curl_axis = fingers.cross(palm).normalized()
      axes["curl"] = (to_local @ curl_axis).normalized()
      axes["spread"] = (to_local @ palm).normalized()
      axes["twist"] = (to_local @ fingers).normalized()
  return axes


def PoseBones(armature, table):
  for bone_name, turns in table.items():
    pose_bone = armature.pose.bones[bone_name]
    axes = BoneAxes(armature, bone_name)
    rotation = Quaternion()
    for axis, degrees in turns:
      rotation = Quaternion(axes[axis], math.radians(degrees)) @ rotation
    pose_bone.rotation_mode = "QUATERNION"
    pose_bone.rotation_quaternion = rotation
  bpy.context.view_layer.update()


def HandSocket(armature, side):
  """Palm centre and hand frame of the posed hand, figure space."""
  pose = armature.pose.bones
  world = armature.matrix_world
  wrist = world @ pose[f"{side}_Wrist_Joint_01"].head
  middle = world @ pose[f"{side}_Middle_Finger_Joint_01a"].head
  index = world @ pose[f"{side}_Index_Finger_Joint_01a"].head
  pinky = world @ pose[f"{side}_Pinky_Finger_Joint_01a"].head
  thumb = world @ pose[f"{side}_Thumb_Joint_01b"].head
  fingers = ToEngine(middle) - ToEngine(wrist)
  across = ToEngine(pinky) - ToEngine(index)
  palm = fingers.cross(across)
  if palm.dot(ToEngine(thumb) - (ToEngine(wrist) + ToEngine(middle)) *
              0.5) < 0.0:
    palm = -palm
  frame = FrameMatrix(fingers, palm)  # columns: fingers, palm, third
  # Socket convention: +Z along the fingers, +Y the palm normal, +X = Y x Z.
  fingers, palm = frame.col[0], frame.col[1]
  socket = Matrix((palm.cross(fingers), palm, fingers)).transposed()
  centre = ((ToEngine(wrist) + ToEngine(middle)) * 0.5 +
            frame.col[1] * PALM_DEPTH)
  return centre, socket.to_quaternion()


def BakeMesh(mesh_object, name):
  """Returns a new object holding the posed mesh in engine-ready form:
  Blender world space, origin at the centre between the feet."""
  depsgraph = bpy.context.evaluated_depsgraph_get()
  evaluated = mesh_object.evaluated_get(depsgraph)
  baked = bpy.data.meshes.new_from_object(evaluated)
  baked.transform(mesh_object.matrix_world)
  baked.name = name
  feet = [v.co for v in baked.vertices if v.co.z < 0.03]
  centre = Vector((sum(v.x for v in feet) / len(feet),
                   sum(v.y for v in feet) / len(feet), 0.0))
  min_z = min(v.co.z for v in baked.vertices)
  offset = Vector((centre.x, centre.y, min_z))
  baked.transform(Matrix.Translation(-offset))
  material = bpy.data.materials.new("kyle")
  material.diffuse_color = FIGURE_MATERIAL
  material.use_nodes = True
  bsdf = next(n for n in material.node_tree.nodes
              if n.type == "BSDF_PRINCIPLED")
  bsdf.inputs["Base Color"].default_value = FIGURE_MATERIAL
  bsdf.inputs["Metallic"].default_value = 0.6
  bsdf.inputs["Roughness"].default_value = 0.45
  baked.materials.clear()
  baked.materials.append(material)
  result = bpy.data.objects.new(name, baked)
  bpy.context.scene.collection.objects.link(result)
  return result, ToEngine(offset) - ToEngine(Vector((0, 0, 0)))


def ExportGlb(obj, path):
  bpy.ops.object.select_all(action="DESELECT")
  obj.select_set(True)
  bpy.context.view_layer.objects.active = obj
  bpy.ops.export_scene.gltf(filepath=path, export_format="GLB",
                            use_selection=True, export_yup=True,
                            export_apply=True, export_normals=True,
                            export_texcoords=True, export_materials="EXPORT",
                            export_animations=False, export_skins=False)


def F(value):
  return f"{value:.6f}".rstrip("0").rstrip(".") if value != 0 else "0"


def WritePose(path, pose_name, weapon, matrix, hands):
  translation, rotation, _ = matrix.decompose()
  lines = [
      "# Generated by tools/assets/kyle/bake_kyle_poses.py; do not edit.",
      f"# Pose '{pose_name}'. Figure space: metres, +Y up, the figure faces",
      "# -Z (its right is +X), origin = centre between the feet on the",
      "# ground. Rotations are quaternions w x y z.",
      "# weapon.*: the weapon prefab (prefab space) placed in the figure.",
      "# hand_r / hand_l: palm centre; +Z along the fingers, +Y the palm",
      "# normal.",
      f"weapon = {WEAPONS[weapon]['prefab']}",
      "weapon.translation = " + " ".join(F(v) for v in translation),
      "weapon.rotation = " + " ".join(F(v) for v in rotation),
  ]
  for key, (centre, frame) in hands.items():
    lines.append(f"{key}.translation = " + " ".join(F(v) for v in centre))
    lines.append(f"{key}.rotation = " + " ".join(F(v) for v in frame))
  with open(path, "w", newline="\n") as out:
    out.write("\n".join(lines) + "\n")


def SetUpRender():
  scene = bpy.context.scene
  try:
    scene.render.engine = "BLENDER_WORKBENCH"
  except TypeError as error:
    print("render engine:", error)
  scene.render.resolution_x = 640
  scene.render.resolution_y = 640
  scene.display.shading.light = "STUDIO"
  scene.display.shading.color_type = "MATERIAL"
  scene.display.shading.show_shadows = False
  scene.display.shading.show_cavity = True
  scene.render.image_settings.file_format = "PNG"
  world = bpy.data.worlds.new("preview")
  scene.world = world
  camera_data = bpy.data.cameras.new("preview")
  camera_data.lens = 50
  camera = bpy.data.objects.new("preview", camera_data)
  scene.collection.objects.link(camera)
  scene.camera = camera
  # Ground plane for reference.
  bpy.ops.mesh.primitive_plane_add(size=3.0, location=(0, 0, 0))
  return camera


# Close-ups of each hand: camera direction from the palm centre, figure space.
CLOSE_UPS = {
    "hand_r": [V(1.0, 0.3, -0.4), V(-0.3, -0.6, -1.0)],
    "hand_l": [V(-1.0, 0.2, -0.6), V(0.3, -0.5, -1.0)],
}
CLOSE_UP_DISTANCE = 0.65


def Render(camera, name, hands):
  views = dict(VIEWS)
  for key, directions in CLOSE_UPS.items():
    centre = hands[key][0]
    for i, direction in enumerate(directions):
      views[f"{key}_{i}"] = (
          centre + direction.normalized() * CLOSE_UP_DISTANCE, centre)
  for view, (eye, target) in views.items():
    eye_b = ToBlender(eye)
    target_b = ToBlender(target)
    camera.location = eye_b
    direction = target_b - eye_b
    camera.rotation_mode = "QUATERNION"
    camera.rotation_quaternion = direction.to_track_quat("-Z", "Y")
    bpy.context.scene.render.filepath = os.path.join(
        PREVIEW_DIR, f"{name}_{view}.png")
    bpy.ops.render.render(write_still=True)


def Report(armature, name, wrists):
  for side, wanted in wrists.items():
    got = ToEngine(armature.matrix_world @
                   armature.pose.bones[f"{side}_Wrist_Joint_01"].head)
    bones = armature.data.bones
    upper = bones[f"{side}_Upper_Arm_Joint_01"]
    forearm = bones[f"{side}_Forearm_Joint_01"]
    shoulder = ToEngine(armature.matrix_world @
                        armature.pose.bones[upper.name].head)
    reach = (upper.length + forearm.length) * armature.matrix_world.to_scale().x
    print(f"[{name}] {side} wrist error {(got - wanted).length * 1000:.1f} mm,"
          f" reach {(wanted - shoulder).length:.3f} of {reach:.3f} m")


def BakePose(fbx, name, pose):
  ResetScene()
  armature, mesh = ImportKyle(fbx)
  holder = ImportWeapon(pose["weapon"])
  weapon_matrix = WeaponMatrix(pose)
  scale = WEAPONS[pose["weapon"]]["scale"]
  holder.matrix_world = (ENGINE_TO_BLENDER @ weapon_matrix @
                         Matrix.Scale(scale, 4) @ BLENDER_TO_ENGINE)
  bones = dict(pose["bones"])
  bones["Root"] = [("y", pose["body_turn"])] + bones.get("Root", [])
  PoseBones(armature, bones)
  wrists = {}
  for side, hand in pose["hands"].items():
    forearm, ik, pole, wrists[side] = PoseArm(armature, side, hand,
                                              weapon_matrix, BodyTurn(pose))
    PickPoleAngle(armature, forearm, ik, pole)
  bpy.context.view_layer.update()
  Report(armature, name, wrists)

  hands = {"hand_r": HandSocket(armature, "Right"),
           "hand_l": HandSocket(armature, "Left")}
  baked, offset = BakeMesh(mesh, name)
  # Everything (weapon, hands) is relative to the baked origin.
  shift = Matrix.Translation(-offset)
  weapon_figure = shift @ weapon_matrix
  hands = {k: (c - offset, q) for k, (c, q) in hands.items()}
  print(f"[{name}] feet offset {tuple(round(v, 4) for v in offset)}")
  holder.matrix_world = (ENGINE_TO_BLENDER @ weapon_figure @
                         Matrix.Scale(scale, 4) @ BLENDER_TO_ENGINE)

  os.makedirs(OUT_DIR, exist_ok=True)
  ExportGlb(baked, os.path.join(OUT_DIR, name + ".glb"))
  WritePose(os.path.join(OUT_DIR, name + ".pose"), name, pose["weapon"],
            weapon_figure, hands)

  mesh.hide_render = True
  armature.hide_render = True
  camera = SetUpRender()
  Render(camera, name, hands)


def Main():
  argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
  fbx = argv[0] if argv else DEFAULT_FBX
  only = argv[1:]
  os.makedirs(PREVIEW_DIR, exist_ok=True)
  for name, pose in POSES.items():
    if only and name not in only:
      continue
    BakePose(fbx, name, pose)


Main()
