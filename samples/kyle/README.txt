Robot Kyle figures (samples/kyle/)
==================================

kyle_rifle.glb, kyle_pistol.glb   static meshes of "Robot Kyle" posed holding
                                  the sample M16 / Glock 17
kyle_rifle.pose, kyle_pistol.pose where the weapon and the hands sit
                                  (format: src/cook/pose_file.h)
kyle.bmat                         the plain material the cook uses instead of
                                  the model's
kyle.glb                          the animated player model: one rigid part
                                  per bone on the bone hierarchy, fingers
                                  baked closed, joint caps
                                  (tools/assets/kyle/bake_kyle_rig.py)

Source: "Robot Kyle.fbx" (the "Robot Kyle" model by Unity Technologies; rig of
49 bones, mesh "Robot2"), posed and baked by
tools/assets/kyle/bake_kyle_poses.py. Its textures were not available, hence
the plain material.

LICENCE NOT VERIFIED - do not redistribute until checked. This model is not
CC0 and is deliberately not listed in samples/ASSETS.txt.
