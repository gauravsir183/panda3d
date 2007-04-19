// Filename: collisionNode.cxx
// Created by:  drose (16Mar02)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) 2001 - 2004, Disney Enterprises, Inc.  All rights reserved
//
// All use of this software is subject to the terms of the Panda 3d
// Software license.  You should have received a copy of this license
// along with this source code; you will also find a current copy of
// the license at http://etc.cmu.edu/panda3d/docs/license/ .
//
// To contact the maintainers of this program write to
// panda3d-general@lists.sourceforge.net .
//
////////////////////////////////////////////////////////////////////

#include "collisionNode.h"
#include "config_collide.h"

#include "geomNode.h"
#include "cullTraverserData.h"
#include "cullTraverser.h"
#include "renderState.h"
#include "transformState.h"
#include "colorScaleAttrib.h"
#include "transparencyAttrib.h"
#include "datagram.h"
#include "datagramIterator.h"
#include "bamReader.h"
#include "bamWriter.h"
#include "clockObject.h"

TypeHandle CollisionNode::_type_handle;


////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::Constructor
//       Access: Public
//  Description:
////////////////////////////////////////////////////////////////////
CollisionNode::
CollisionNode(const string &name) :
  PandaNode(name),
  _from_collide_mask(get_default_collide_mask()),
  _collider_sort(0)
{
  set_cull_callback();

  // CollisionNodes are hidden by default.
  set_overall_hidden(true);

  // CollisionNodes have a certain set of bits on by default.
  set_into_collide_mask(get_default_collide_mask());
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::Copy Constructor
//       Access: Protected
//  Description:
////////////////////////////////////////////////////////////////////
CollisionNode::
CollisionNode(const CollisionNode &copy) :
  PandaNode(copy),
  _from_collide_mask(copy._from_collide_mask),
  _solids(copy._solids)
{
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::Destructor
//       Access: Public, Virtual
//  Description:
////////////////////////////////////////////////////////////////////
CollisionNode::
~CollisionNode() {
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::make_copy
//       Access: Public, Virtual
//  Description: Returns a newly-allocated Node that is a shallow copy
//               of this one.  It will be a different Node pointer,
//               but its internal data may or may not be shared with
//               that of the original Node.
////////////////////////////////////////////////////////////////////
PandaNode *CollisionNode::
make_copy() const {
  return new CollisionNode(*this);
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::preserve_name
//       Access: Public, Virtual
//  Description: Returns true if the node's name has extrinsic meaning
//               and must be preserved across a flatten operation,
//               false otherwise.
////////////////////////////////////////////////////////////////////
bool CollisionNode::
preserve_name() const {
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::xform
//       Access: Public, Virtual
//  Description: Transforms the contents of this node by the indicated
//               matrix, if it means anything to do so.  For most
//               kinds of nodes, this does nothing.
////////////////////////////////////////////////////////////////////
void CollisionNode::
xform(const LMatrix4f &mat) {
  nassertv(!mat.is_nan());

  if (mat.almost_equal(LMatrix4f::ident_mat())) {
    return;
  }

  Solids::iterator si;
  for (si = _solids.begin(); si != _solids.end(); ++si) {
    PT(CollisionSolid) solid = (*si).get_write_pointer();
    solid->xform(mat);
  }
  mark_internal_bounds_stale();
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::combine_with
//       Access: Public, Virtual
//  Description: Collapses this node with the other node, if possible,
//               and returns a pointer to the combined node, or NULL
//               if the two nodes cannot safely be combined.
//
//               The return value may be this, other, or a new node
//               altogether.
//
//               This function is called from GraphReducer::flatten(),
//               and need not deal with children; its job is just to
//               decide whether to collapse the two nodes and what the
//               collapsed node should look like.
////////////////////////////////////////////////////////////////////
PandaNode *CollisionNode::
combine_with(PandaNode *other) {
  if (is_exact_type(get_class_type()) &&
      other->is_exact_type(get_class_type())) {
    // Two CollisionNodes can combine, but only if they have the same
    // name, because the name is often meaningful.
    CollisionNode *cother = DCAST(CollisionNode, other);
    if (get_name() == cother->get_name()) {
      const COWPT(CollisionSolid) *solids_begin = &cother->_solids[0];
      const COWPT(CollisionSolid) *solids_end = solids_begin + cother->_solids.size();
      _solids.insert(_solids.end(), solids_begin, solids_end);
      mark_internal_bounds_stale();
      return this;
    }

    // Two CollisionNodes with different names can't combine.
    return (PandaNode *)NULL;
  }

  return PandaNode::combine_with(other);
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::get_legal_collide_mask
//       Access: Published, Virtual
//  Description: Returns the subset of CollideMask bits that may be
//               set for this particular type of PandaNode.  For most
//               nodes, this is 0; it doesn't make sense to set a
//               CollideMask for most kinds of nodes.
//
//               For nodes that can be collided with, such as GeomNode
//               and CollisionNode, this returns all bits on.
////////////////////////////////////////////////////////////////////
CollideMask CollisionNode::
get_legal_collide_mask() const {
  return CollideMask::all_on();
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::cull_callback
//       Access: Public, Virtual
//  Description: This function will be called during the cull
//               traversal to perform any additional operations that
//               should be performed at cull time.  This may include
//               additional manipulation of render state or additional
//               visible/invisible decisions, or any other arbitrary
//               operation.
//
//               Note that this function will *not* be called unless
//               set_cull_callback() is called in the constructor of
//               the derived class.  It is necessary to call
//               set_cull_callback() to indicated that we require
//               cull_callback() to be called.
//
//               By the time this function is called, the node has
//               already passed the bounding-volume test for the
//               viewing frustum, and the node's transform and state
//               have already been applied to the indicated
//               CullTraverserData object.
//
//               The return value is true if this node should be
//               visible, or false if it should be culled.
////////////////////////////////////////////////////////////////////
bool CollisionNode::
cull_callback(CullTraverser *trav, CullTraverserData &data) {
  // Append our collision vizzes to the drawing, even though they're
  // not actually part of the scene graph.
  Solids::const_iterator si;
  for (si = _solids.begin(); si != _solids.end(); ++si) {
    CPT(CollisionSolid) solid = (*si).get_read_pointer();
    PT(PandaNode) node = solid->get_viz(trav, data, false);
    if (node != (PandaNode *)NULL) {
      CullTraverserData next_data(data, node);

      // We don't want to inherit the render state from above for these
      // guys.
      next_data._state = RenderState::make_empty();
      trav->traverse(next_data);
    }
  }

  if (respect_prev_transform) {
    // Determine the previous frame's position, relative to the
    // current position.
    NodePath node_path = data._node_path.get_node_path();
    CPT(TransformState) transform = node_path.get_net_transform()->invert_compose(node_path.get_net_prev_transform());
    
    if (!transform->is_identity()) {
      // If we have a velocity, also draw the previous frame's position,
      // ghosted.
      
      for (si = _solids.begin(); si != _solids.end(); ++si) {
        CPT(CollisionSolid) solid = (*si).get_read_pointer();
        PT(PandaNode) node = solid->get_viz(trav, data, false);
        if (node != (PandaNode *)NULL) {
          CullTraverserData next_data(data, node);
          
          next_data._net_transform = 
            next_data._net_transform->compose(transform);
          next_data._state = get_last_pos_state();
          trav->traverse(next_data);
        }
      }
    }
  }

  // Now carry on to render our child nodes.
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::is_renderable
//       Access: Public, Virtual
//  Description: Returns true if there is some value to visiting this
//               particular node during the cull traversal for any
//               camera, false otherwise.  This will be used to
//               optimize the result of get_net_draw_show_mask(), so
//               that any subtrees that contain only nodes for which
//               is_renderable() is false need not be visited.
////////////////////////////////////////////////////////////////////
bool CollisionNode::
is_renderable() const {
  return true;
}


////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::output
//       Access: Public, Virtual
//  Description: Writes a brief description of the node to the
//               indicated output stream.  This is invoked by the <<
//               operator.  It may be overridden in derived classes to
//               include some information relevant to the class.
////////////////////////////////////////////////////////////////////
void CollisionNode::
output(ostream &out) const {
  PandaNode::output(out);
  out << " (" << _solids.size() << " solids)";
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::set_from_collide_mask
//       Access: Published
//  Description: Sets the "from" CollideMask.  In order for a
//               collision to be detected from this object into
//               another object, the intersection of this object's
//               "from" mask and the other object's "into" mask must
//               be nonzero.
////////////////////////////////////////////////////////////////////
void CollisionNode::
set_from_collide_mask(CollideMask mask) {
  _from_collide_mask = mask;
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::compute_internal_bounds
//       Access: Protected, Virtual
//  Description: Called when needed to recompute the node's
//               _internal_bound object.  Nodes that contain anything
//               of substance should redefine this to do the right
//               thing.
////////////////////////////////////////////////////////////////////
PT(BoundingVolume) CollisionNode::
compute_internal_bounds(int pipeline_stage, Thread *current_thread) const {
  // First, get ourselves a fresh, empty bounding volume.
  PT(BoundingVolume) bound = PandaNode::compute_internal_bounds(pipeline_stage, current_thread);
  nassertr(bound != (BoundingVolume *)NULL, bound);

  // Now actually compute the bounding volume by putting it around all
  // of our solids' bounding volumes.
  pvector<const BoundingVolume *> child_volumes;
  pvector<CPT(BoundingVolume) > cpt_volumes;

  Solids::const_iterator gi;
  for (gi = _solids.begin(); gi != _solids.end(); ++gi) {
    CPT(CollisionSolid) solid = (*gi).get_read_pointer();
    CPT(BoundingVolume) volume = solid->get_bounds();
    cpt_volumes.push_back(volume);
    child_volumes.push_back(volume);
  }

  const BoundingVolume **child_begin = &child_volumes[0];
  const BoundingVolume **child_end = child_begin + child_volumes.size();

  bool success =
    bound->around(child_begin, child_end);

#ifdef NOTIFY_DEBUG
  if (!success) {
    collide_cat.error()
      << "Unable to generate bounding volume for " << *this << ":\n"
      << "Cannot put " << bound->get_type() << " around:\n";
    for (int i = 0; i < (int)child_volumes.size(); i++) {
      collide_cat.error(false)
        << "  " << *child_volumes[i] << "\n";
    }
  }
#endif

  return bound;
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::get_last_pos_state
//       Access: Protected
//  Description: Returns a RenderState for rendering the ghosted
//               collision solid that represents the previous frame's
//               position, for those collision nodes that indicate a
//               velocity.
////////////////////////////////////////////////////////////////////
CPT(RenderState) CollisionNode::
get_last_pos_state() {
  // Once someone asks for this pointer, we hold its reference count
  // and never free it.
  static CPT(RenderState) state = (const RenderState *)NULL;
  if (state == (const RenderState *)NULL) {
    state = RenderState::make
      (ColorScaleAttrib::make(LVecBase4f(1.0f, 1.0f, 1.0f, 0.5f)),
       TransparencyAttrib::make(TransparencyAttrib::M_alpha));
  }

  return state;
}


////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::register_with_read_factory
//       Access: Public, Static
//  Description: Tells the BamReader how to create objects of type
//               CollisionNode.
////////////////////////////////////////////////////////////////////
void CollisionNode::
register_with_read_factory() {
  BamReader::get_factory()->register_factory(get_class_type(), make_from_bam);
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::write_datagram
//       Access: Public, Virtual
//  Description: Writes the contents of this object to the datagram
//               for shipping out to a Bam file.
////////////////////////////////////////////////////////////////////
void CollisionNode::
write_datagram(BamWriter *manager, Datagram &dg) {
  PandaNode::write_datagram(manager, dg);

  int num_solids = _solids.size();
  dg.add_uint16(num_solids);
  for(int i = 0; i < num_solids; i++) {
    manager->write_pointer(dg, _solids[i].get_read_pointer());
  }

  dg.add_uint32(_from_collide_mask.get_word());
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::complete_pointers
//       Access: Public, Virtual
//  Description: Receives an array of pointers, one for each time
//               manager->read_pointer() was called in fillin().
//               Returns the number of pointers processed.
////////////////////////////////////////////////////////////////////
int CollisionNode::
complete_pointers(TypedWritable **p_list, BamReader *manager) {
  int pi = PandaNode::complete_pointers(p_list, manager);

  int num_solids = _solids.size();
  for (int i = 0; i < num_solids; i++) {
    _solids[i] = DCAST(CollisionSolid, p_list[pi++]);
  }

  return pi;
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::make_from_bam
//       Access: Protected, Static
//  Description: This function is called by the BamReader's factory
//               when a new object of type CollisionNode is encountered
//               in the Bam file.  It should create the CollisionNode
//               and extract its information from the file.
////////////////////////////////////////////////////////////////////
TypedWritable *CollisionNode::
make_from_bam(const FactoryParams &params) {
  CollisionNode *node = new CollisionNode("");
  DatagramIterator scan;
  BamReader *manager;

  parse_params(params, scan, manager);
  node->fillin(scan, manager);

  return node;
}

////////////////////////////////////////////////////////////////////
//     Function: CollisionNode::fillin
//       Access: Protected
//  Description: This internal function is called by make_from_bam to
//               read in all of the relevant data from the BamFile for
//               the new CollisionNode.
////////////////////////////////////////////////////////////////////
void CollisionNode::
fillin(DatagramIterator &scan, BamReader *manager) {
  PandaNode::fillin(scan, manager);

  int num_solids = scan.get_uint16();
  _solids.clear();
  _solids.reserve(num_solids);
  for(int i = 0; i < num_solids; i++) {
    manager->read_pointer(scan);
    // Push back a NULL for each solid, for now.  We'll fill them in
    // later.
    _solids.push_back((CollisionSolid *)NULL);
  }

  _from_collide_mask.set_word(scan.get_uint32());
}
