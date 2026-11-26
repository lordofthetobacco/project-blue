"""Bakes "Robot Kyle" into rigid body parts for run-time animation.

Run headless (Blender 5.3):

  blender -b --python tools/assets/kyle/bake_kyle_rig.py
      [-- [<Robot Kyle.fbx>] [--no-preview]]

(default FBX: ~/Downloads/Robot Kyle.fbx). Only reads the FBX; nothing is
saved as .blend.

The engine animates rigid nodes only, and Kyle is a robot: 94.6 % of its
vertices follow exactly one bone. So every vertex goes to its dominant bone,
the fingers are baked into the hands in a closed grip (right: pistol grip,
left: support grip), and each kept bone becomes one node carrying one mesh
part. Writes samples/kyle/kyle.glb:

  nodes   Root, Hip, Ribs, Neck, Head and per side (Left_/Right_) Shoulder,
          Upper_Arm, Forearm, Wrist, Thigh, Knee, Ankle, Toe: the bone
          hierarchy. A node's origin is its bone's rest head, its rotation
          is the identity (axes = figure axes), so a node's rest
          translation is the vector from its parent's joint: the lengths
          the run-time IK needs (upper arm = |Left_Forearm|, ...) are read
          from the cooked hierarchy, never typed twice. Joint caps (small
          spheres under a seam) are extra meshes of the nodes they belong
          to.
  sockets empty child nodes hand_r, hand_l (palm centre; +Z along the flat
          hand's fingers, +Y the palm normal, as the N15 figures), head
          (top of the head) and chest (front of the chest); `blue-pack
          samples` turns them into prefab sockets.
  space   engine figure space: metres, +Y up, the figure faces -Z (its
          right is +X), origin = centre between the feet on the ground.

Preview renders of the parts in bent poses (Workbench) go to
out/tools/logs/kyle/rig/<pose>_<view>.png, for checking the seams.
"""

import json
import math
import os
import struct
import sys

import bpy
from mathutils import Matrix, Quaternion, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
OUT_PATH = os.path.join(REPO, "samples", "kyle", "kyle.glb")
PREVIEW_DIR = os.path.join(REPO, "out", "tools", "logs", "kyle", "rig")
DEFAULT_FBX = os.path.join(os.path.expanduser("~"), "Downloads",
                           "Robot Kyle.fbx")

# Kept bones: node name -> FBX bone. Parents follow the FBX hierarchy.
SIDES = ("Left", "Right")
LIMB_BONES = ("Shoulder", "Upper_Arm", "Forearm", "Wrist", "Thigh", "Knee",
              "Ankle", "Toe")
NODES = ["Root", "Hip", "Ribs", "Neck", "Head"] + [
    f"{side}_{bone}" for side in SIDES for bone in LIMB_BONES
]


def BoneOf(node):
  if node in ("Root", "Hip", "Ribs", "Neck", "Head"):
    return node
  return node + "_Joint_01"


def NodeOfBone(bone_name):
  """The kept node a bone's vertices go to (fingers -> the hand)."""
  for node in NODES:
    if BoneOf(node) == bone_name:
      return node
  for side in SIDES:
    if bone_name.startswith(side) and ("Finger" in bone_name or
                                       "Thumb" in bone_name):
      return f"{side}_Wrist"
  raise KeyError(bone_name)


# Finger curls (degrees over the three joints) and thumb curls of the closed
# hands: the N15 figures' grip (tools/assets/kyle/bake_kyle_poses.py).
FINGERS_GRIP = {
    "Index": (40, 55, 30),
    "Middle": (45, 60, 30),
    "Ring": (45, 60, 30),
    "Pinky": (45, 60, 30),
}
THUMB_CURL = {"Right": (25.0, 25.0), "Left": (10.0, 10.0)}
# The palm centre sits this far from the midpoint between the wrist joint and
# the middle finger's knuckle, towards the palm (m).
PALM_DEPTH = 0.015

# Joint caps: (node, joint node whose head is the centre, radius m). Spheres
# that fill the seams a rigid split opens when the joint bends; tuned by
# looking at the bent-pose previews.
JOINT_CAPS = [
    ("Hip", "Left_Thigh", 0.075),
    ("Hip", "Right_Thigh", 0.075),
    ("Left_Thigh", "Left_Knee", 0.062),
    ("Right_Thigh", "Right_Knee", 0.062),
    ("Left_Upper_Arm", "Left_Upper_Arm", 0.062),
    ("Right_Upper_Arm", "Right_Upper_Arm", 0.062),
    ("Left_Upper_Arm", "Left_Forearm", 0.048),
    ("Right_Upper_Arm", "Right_Forearm", 0.048),
    ("Hip", "Ribs", 0.085),
    ("Left_Knee", "Left_Ankle", 0.036),
    ("Right_Knee", "Right_Ankle", 0.036),
]
CAP_SEGMENTS = 16
# Triangles straddling these pairs of parts are left out: kept whole on one
# side they stick out as spikes once the joint bends. The joint caps above
# cover the openings.
# (The waist's straddling triangles are its skin: dropping them opens a
# hole; the animator bends the waist little instead.)
DROPPED_SEAMS = {
    ("Left_Ankle", "Left_Knee"),
    ("Right_Ankle", "Right_Knee"),
}


def EngineFromBlender(point):
  """Blender world (FBX import: Z up, the figure faces -Y) -> engine figure
  space before the feet offset (the figure turned to face -Z)."""
  return Vector((-point.x, point.z, point.y))


# ---------------------------------------------------------------------------
# Blender side


def ResetScene():
  bpy.ops.wm.read_factory_settings(use_empty=True)


def ImportKyle(fbx):
  bpy.ops.import_scene.fbx(filepath=fbx, automatic_bone_orientation=True)
  armature = next(o for o in bpy.data.objects if o.type == "ARMATURE")
  mesh = next(o for o in bpy.data.objects if o.type == "MESH")
  return armature, mesh


def RestHandFrame(armature, side):
  """Finger direction, palm normal and across (index -> pinky) of a hand at
  rest, armature space (as bake_kyle_poses.py)."""
  bones = armature.data.bones
  wrist = bones[f"{side}_Wrist_Joint_01"].head_local
  middle = bones[f"{side}_Middle_Finger_Joint_01a"].head_local
  index = bones[f"{side}_Index_Finger_Joint_01a"].head_local
  pinky = bones[f"{side}_Pinky_Finger_Joint_01a"].head_local
  thumb = bones[f"{side}_Thumb_Joint_01b"].head_local
  fingers = (middle - wrist).normalized()
  across = pinky - index
  across = (across - fingers * across.dot(fingers)).normalized()
  palm = fingers.cross(across).normalized()
  if palm.dot(thumb - (wrist + middle) * 0.5) < 0.0:
    palm = -palm
  return fingers, palm, across


def CloseHands(armature):
  """Curls the fingers and thumbs into the grip (pose mode rotations)."""
  for side in SIDES:
    fingers, palm, _ = RestHandFrame(armature, side)
    curl_axis = fingers.cross(palm).normalized()
    curls = {}
    for finger, (a, b, c) in FINGERS_GRIP.items():
      base = f"{side}_{finger}_Finger_Joint_01"
      curls[base + "a"] = a
      curls[base + "b"] = b
      curls[base + "c"] = c
    curls[f"{side}_Thumb_Joint_01a"] = THUMB_CURL[side][0]
    curls[f"{side}_Thumb_Joint_01b"] = THUMB_CURL[side][1]
    for bone_name, degrees in curls.items():
      bone = armature.data.bones[bone_name]
      axis = (bone.matrix_local.to_3x3().inverted() @ curl_axis).normalized()
      pose_bone = armature.pose.bones[bone_name]
      pose_bone.rotation_mode = "QUATERNION"
      pose_bone.rotation_quaternion = Quaternion(axis, math.radians(degrees))
  bpy.context.view_layer.update()


def EvaluatedMesh(mesh_object):
  """The posed (closed-hand) mesh, triangulated, Blender world space."""
  depsgraph = bpy.context.evaluated_depsgraph_get()
  evaluated = mesh_object.evaluated_get(depsgraph)
  data = bpy.data.meshes.new_from_object(evaluated)
  data.transform(mesh_object.matrix_world)
  data.calc_loop_triangles()
  return data


def DominantNodes(mesh_object):
  groups = {g.index: g.name for g in mesh_object.vertex_groups}
  dominant = []
  for vertex in mesh_object.data.vertices:
    best = max(((g.weight, groups[g.group]) for g in vertex.groups),
               default=(0.0, "Ribs"))
    dominant.append(NodeOfBone(best[1]))
  return dominant


def JointHeads(armature):
  """Rest head of every kept node's bone, engine space (no feet offset)."""
  world = armature.matrix_world
  return {node: EngineFromBlender(world @ armature.data.bones[BoneOf(node)]
                                  .head_local)
          for node in NODES}


def Parents(armature):
  parents = {}
  for node in NODES:
    bone = armature.data.bones[BoneOf(node)].parent
    while bone is not None and bone.name not in [BoneOf(n) for n in NODES]:
      bone = bone.parent
    parents[node] = NodeOfBone(bone.name) if bone is not None else None
  return parents


def HandSocket(armature, side):
  """Palm centre and frame (+Z fingers, +Y palm normal) of a hand at rest,
  engine space (no feet offset)."""
  world = armature.matrix_world
  bones = armature.data.bones
  wrist = EngineFromBlender(world @ bones[f"{side}_Wrist_Joint_01"]
                            .head_local)
  middle = EngineFromBlender(world @ bones[f"{side}_Middle_Finger_Joint_01a"]
                             .head_local)
  index = EngineFromBlender(world @ bones[f"{side}_Index_Finger_Joint_01a"]
                            .head_local)
  pinky = EngineFromBlender(world @ bones[f"{side}_Pinky_Finger_Joint_01a"]
                            .head_local)
  thumb = EngineFromBlender(world @ bones[f"{side}_Thumb_Joint_01b"]
                            .head_local)
  fingers = (middle - wrist).normalized()
  across = pinky - index
  palm = fingers.cross(across)
  palm = (palm - fingers * palm.dot(fingers)).normalized()
  if palm.dot(thumb - (wrist + middle) * 0.5) < 0.0:
    palm = -palm
  x = palm.cross(fingers).normalized()
  rotation = Matrix((x, palm, fingers)).transposed().to_quaternion()
  centre = (wrist + middle) * 0.5 + palm * PALM_DEPTH
  return centre, rotation


# ---------------------------------------------------------------------------
# Parts


class Part:
  def __init__(self):
    self.positions = []  # engine space, absolute (with the feet offset)
    self.normals = []
    self.uvs = []
    self.indices = []
    self.lookup = {}

  def Add(self, position, normal, uv):
    key = (tuple(round(v, 6) for v in position),
           tuple(round(v, 4) for v in normal), tuple(round(v, 5) for v in uv))
    index = self.lookup.get(key)
    if index is None:
      index = len(self.positions)
      self.lookup[key] = index
      self.positions.append(Vector(position))
      self.normals.append(Vector(normal))
      self.uvs.append(uv)
    self.indices.append(index)


def SplitParts(data, dominant, offset):
  """One part per node; returns the parts, the triangle count per node and
  the count of triangles straddling each pair of nodes."""
  parts = {node: Part() for node in NODES}
  uv_layer = data.uv_layers.active.data if data.uv_layers.active else None
  counts = {}
  straddling = {}
  for triangle in data.loop_triangles:
    votes = {}
    for vertex in triangle.vertices:
      votes[dominant[vertex]] = votes.get(dominant[vertex], 0) + 1
    if len(votes) > 1:
      pair = tuple(sorted(votes))
      straddling[pair] = straddling.get(pair, 0) + 1
      if pair in DROPPED_SEAMS:
        continue
    node = max(votes.items(), key=lambda item: (item[1], item[0]))[0]
    counts[node] = counts.get(node, 0) + 1
    part = parts[node]
    for corner, loop in zip(triangle.vertices, triangle.loops):
      position = EngineFromBlender(data.vertices[corner].co) - offset
      normal = EngineFromBlender(Vector(data.corner_normals[loop].vector))
      uv = (0.0, 0.0)
      if uv_layer is not None:
        u, v = uv_layer[loop].uv
        uv = (u, 1.0 - v)  # glTF: v down
      part.Add(position, normal.normalized(), uv)
  return parts, counts, straddling


def AddSphere(part, centre, radius):
  first = len(part.positions)
  rings = CAP_SEGMENTS // 2
  for ring in range(rings + 1):
    theta = math.pi * ring / rings
    for segment in range(CAP_SEGMENTS + 1):
      phi = 2.0 * math.pi * segment / CAP_SEGMENTS
      normal = Vector((math.sin(theta) * math.cos(phi), math.cos(theta),
                       math.sin(theta) * math.sin(phi)))
      part.positions.append(centre + normal * radius)
      part.normals.append(normal)
      part.uvs.append((segment / CAP_SEGMENTS, ring / rings))
  stride = CAP_SEGMENTS + 1
  for ring in range(rings):
    for segment in range(CAP_SEGMENTS):
      a = first + ring * stride + segment
      b = a + stride
      part.indices += [a, a + 1, b, b, a + 1, b + 1]


# ---------------------------------------------------------------------------
# glTF writer


class GltfWriter:
  def __init__(self):
    self.binary = bytearray()
    self.buffer_views = []
    self.accessors = []

  def _View(self, data, target):
    while len(self.binary) % 4:
      self.binary.append(0)
    self.buffer_views.append({"buffer": 0, "byteOffset": len(self.binary),
                              "byteLength": len(data), "target": target})
    self.binary += data
    return len(self.buffer_views) - 1

  def Floats(self, rows, kind):
    flat = [float(v) for row in rows for v in row]
    view = self._View(struct.pack(f"<{len(flat)}f", *flat), 34962)
    accessor = {"bufferView": view, "componentType": 5126,
                "count": len(rows), "type": kind}
    if kind == "VEC3":
      accessor["min"] = [min(r[i] for r in rows) for i in range(3)]
      accessor["max"] = [max(r[i] for r in rows) for i in range(3)]
    self.accessors.append(accessor)
    return len(self.accessors) - 1

  def Indices(self, indices):
    view = self._View(struct.pack(f"<{len(indices)}I", *indices), 34963)
    self.accessors.append({"bufferView": view, "componentType": 5125,
                           "count": len(indices), "type": "SCALAR"})
    return len(self.accessors) - 1


def WriteGlb(path, heads, parents, parts, sockets):
  writer = GltfWriter()
  nodes = []
  meshes = []
  index_of = {}
  for node in NODES:
    index_of[node] = len(nodes)
    parent = parents[node]
    translation = heads[node] - (heads[parent] if parent else Vector())
    entry = {"name": node, "translation": list(translation)}
    part = parts[node]
    if part.indices:
      local = [p - heads[node] for p in part.positions]
      primitive = {
          "attributes": {
              "POSITION": writer.Floats(local, "VEC3"),
              "NORMAL": writer.Floats(part.normals, "VEC3"),
              "TEXCOORD_0": writer.Floats(part.uvs, "VEC2"),
          },
          "indices": writer.Indices(part.indices),
          "material": 0,
      }
      meshes.append({"name": node, "primitives": [primitive]})
      entry["mesh"] = len(meshes) - 1
    nodes.append(entry)
  for name, (node, centre, rotation) in sockets.items():
    socket = {"name": name, "translation": list(centre - heads[node])}
    if rotation is not None:
      socket["rotation"] = [rotation.x, rotation.y, rotation.z, rotation.w]
    nodes.append(socket)
    nodes[index_of[node]].setdefault("children", []).append(len(nodes) - 1)
  for node in NODES:
    if parents[node]:
      nodes[index_of[parents[node]]].setdefault("children", []).append(
          index_of[node])
  roots = [index_of[n] for n in NODES if not parents[n]]
  while len(writer.binary) % 4:
    writer.binary.append(0)
  document = {
      "asset": {"version": "2.0",
                "generator": "tools/assets/kyle/bake_kyle_rig.py"},
      "scene": 0,
      "scenes": [{"name": "kyle", "nodes": roots}],
      "nodes": nodes,
      "meshes": meshes,
      "materials": [{"name": "kyle", "pbrMetallicRoughness": {
          "baseColorFactor": [0.55, 0.57, 0.6, 1.0], "metallicFactor": 0.0,
          "roughnessFactor": 0.45}}],
      "buffers": [{"byteLength": len(writer.binary)}],
      "bufferViews": writer.buffer_views,
      "accessors": writer.accessors,
  }
  text = json.dumps(document, separators=(",", ":")).encode("utf-8")
  while len(text) % 4:
    text += b" "
  with open(path, "wb") as out:
    total = 12 + 8 + len(text) + 8 + len(writer.binary)
    out.write(struct.pack("<III", 0x46546C67, 2, total))
    out.write(struct.pack("<II", len(text), 0x4E4F534A))
    out.write(text)
    out.write(struct.pack("<II", len(writer.binary), 0x004E4942))
    out.write(writer.binary)


# ---------------------------------------------------------------------------
# Previews: the parts as Blender objects under empties at the joints, posed.

# Bent poses: node -> [(axis, degrees)], rotations in figure space applied in
# the node's (identity-rest) frame. +X = figure right, so a positive turn
# about X lifts a limb forward-up for a node below its joint.
PREVIEW_POSES = {
    "rest": {},
    "knees_hips": {
        "Left_Thigh": [("x", 80.0)], "Left_Knee": [("x", -110.0)],
        "Right_Thigh": [("x", -30.0)], "Right_Knee": [("x", -60.0)],
    },
    "crouch": {
        "Left_Thigh": [("x", 100.0)], "Left_Knee": [("x", -130.0)],
        "Left_Ankle": [("x", 30.0)],
        "Right_Thigh": [("x", 100.0)], "Right_Knee": [("x", -130.0)],
        "Right_Ankle": [("x", 30.0)],
    },
    "arms": {
        "Right_Upper_Arm": [("z", -40.0), ("x", 70.0)],
        "Right_Forearm": [("x", 100.0)],
        "Left_Upper_Arm": [("z", 40.0), ("x", 90.0)],
        "Left_Forearm": [("y", 60.0), ("x", 60.0)],
        "Ribs": [("x", 15.0)],
    },
}
PREVIEW_VIEWS = {
    "front": (Vector((0.0, 1.0, -3.4)), Vector((0.0, 0.9, 0.0))),
    "side": (Vector((3.4, 1.0, 0.0)), Vector((0.0, 0.9, 0.0))),
    "three_quarter": (Vector((-2.2, 1.6, -2.4)), Vector((0.0, 0.9, 0.0))),
    "back": (Vector((1.2, 1.2, 3.0)), Vector((0.0, 0.9, 0.0))),
}


def ToBlender(point):
  """Engine figure space -> Blender (Z up, -Y forward = engine +Z)."""
  return Vector((point.x, -point.z, point.y))


def BuildPreview(heads, parents, parts):
  ResetScene()
  empties = {}
  for node in NODES:
    empty = bpy.data.objects.new("n_" + node, None)
    bpy.context.scene.collection.objects.link(empty)
    parent = parents[node]
    if parent:
      empty.parent = empties[parent]
      empty.location = ToBlender(heads[node] - heads[parent])
    else:
      empty.location = ToBlender(heads[node])
    empty.rotation_mode = "QUATERNION"
    empties[node] = empty
    part = parts[node]
    if not part.indices:
      continue
    mesh = bpy.data.meshes.new(node)
    vertices = [ToBlender(p - heads[node]) for p in part.positions]
    faces = [part.indices[i:i + 3] for i in range(0, len(part.indices), 3)]
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    obj = bpy.data.objects.new(node, mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.parent = empty
    material = bpy.data.materials.new(node)
    shade = 0.45 + 0.4 * ((hash(node) % 7) / 6.0)
    material.diffuse_color = (shade, shade * 0.95, 0.9 * shade, 1.0)
    mesh.materials.append(material)
  return empties


def PoseEmpties(empties, pose):
  axes = {"x": Vector((1, 0, 0)), "y": Vector((0, 1, 0)),
          "z": Vector((0, 0, 1))}
  for node, empty in empties.items():
    rotation = Quaternion()
    for axis, degrees in pose.get(node, []):
      rotation = Quaternion(ToBlender(axes[axis]),
                            math.radians(degrees)) @ rotation
    empty.rotation_quaternion = rotation
  bpy.context.view_layer.update()


def RenderPreviews(heads, parents, parts):
  empties = BuildPreview(heads, parents, parts)
  scene = bpy.context.scene
  try:
    scene.render.engine = "BLENDER_WORKBENCH"
  except TypeError as error:
    print("render engine:", error)
  scene.render.resolution_x = 560
  scene.render.resolution_y = 560
  scene.display.shading.light = "STUDIO"
  scene.display.shading.color_type = "MATERIAL"
  scene.display.shading.show_cavity = True
  scene.render.image_settings.file_format = "PNG"
  scene.world = bpy.data.worlds.new("preview")
  camera_data = bpy.data.cameras.new("preview")
  camera_data.lens = 50
  camera = bpy.data.objects.new("preview", camera_data)
  scene.collection.objects.link(camera)
  scene.camera = camera
  bpy.ops.mesh.primitive_plane_add(size=3.0, location=(0, 0, 0))
  os.makedirs(PREVIEW_DIR, exist_ok=True)
  for pose_name, pose in PREVIEW_POSES.items():
    PoseEmpties(empties, pose)
    for view, (eye, target) in PREVIEW_VIEWS.items():
      camera.location = ToBlender(eye)
      direction = ToBlender(target) - ToBlender(eye)
      camera.rotation_mode = "QUATERNION"
      camera.rotation_quaternion = direction.to_track_quat("-Z", "Y")
      scene.render.filepath = os.path.join(PREVIEW_DIR,
                                           f"{pose_name}_{view}.png")
      bpy.ops.render.render(write_still=True)


# ---------------------------------------------------------------------------


def Main():
  argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
  preview = "--no-preview" not in argv
  argv = [a for a in argv if a != "--no-preview"]
  fbx = argv[0] if argv else DEFAULT_FBX

  ResetScene()
  armature, mesh_object = ImportKyle(fbx)
  dominant = DominantNodes(mesh_object)
  CloseHands(armature)
  data = EvaluatedMesh(mesh_object)

  # Feet offset: the centre between the feet on the ground.
  points = [EngineFromBlender(v.co) for v in data.vertices]
  min_y = min(p.y for p in points)
  feet = [p for p in points if p.y < min_y + 0.03]
  offset = Vector((sum(p.x for p in feet) / len(feet), min_y,
                   sum(p.z for p in feet) / len(feet)))
  height = max(p.y for p in points) - min_y

  heads = {node: head - offset for node, head in JointHeads(armature).items()}
  parents = Parents(armature)
  parts, counts, straddling = SplitParts(data, dominant, offset)
  for pair, count in sorted(straddling.items()):
    print(f"[kyle rig] {count:3d} triangles straddle {pair[0]} / {pair[1]}"
          f"{' (dropped)' if pair in DROPPED_SEAMS else ''}")
  for node, joint, radius in JOINT_CAPS:
    if radius > 0.0:
      AddSphere(parts[node], heads[joint], radius)

  sockets = {}
  for key, side in (("hand_r", "Right"), ("hand_l", "Left")):
    centre, rotation = HandSocket(armature, side)
    sockets[key] = (f"{side}_Wrist", centre - offset, rotation)
  head_part = parts["Head"].positions
  top = max(p.y for p in head_part)
  sockets["head"] = ("Head", Vector((heads["Head"].x, top, heads["Head"].z)),
                     None)
  ribs = parts["Ribs"].positions
  front = min(p.z for p in ribs)
  chest_y = sum(p.y for p in ribs) / len(ribs)
  sockets["chest"] = ("Ribs", Vector((0.0, chest_y, front)), None)

  os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
  WriteGlb(OUT_PATH, heads, parents, parts, sockets)
  print(f"[kyle rig] height {height:.3f} m, feet offset "
        f"{tuple(round(v, 4) for v in offset)}")
  for node in NODES:
    h = heads[node]
    print(f"[kyle rig] {node:16s} parent {str(parents[node]):16s} head "
          f"({h.x:+.4f}, {h.y:+.4f}, {h.z:+.4f}) triangles "
          f"{counts.get(node, 0)}")
  for name, (node, centre, _) in sockets.items():
    print(f"[kyle rig] socket {name} on {node} at "
          f"({centre.x:+.4f}, {centre.y:+.4f}, {centre.z:+.4f})")
  print(f"[kyle rig] wrote {OUT_PATH}")
  if preview:
    RenderPreviews(heads, parents, parts)


Main()
