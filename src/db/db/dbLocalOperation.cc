
/*

  KLayout Layout Viewer
  Copyright (C) 2006-2018 Matthias Koefferlein

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/


#include "dbHierProcessor.h"
#include "dbBoxScanner.h"
#include "dbRecursiveShapeIterator.h"
#include "dbBoxConvert.h"
#include "dbEdgeProcessor.h"
#include "dbPolygonGenerators.h"
#include "dbPolygonTools.h"
#include "tlLog.h"
#include "tlTimer.h"
#include "tlInternational.h"

namespace db
{


// ---------------------------------------------------------------------------------------------
//  BoolAndOrNotLocalOperation implementation

namespace {

class PolygonRefGenerator
  : public PolygonSink
{
public:
  /**
   *  @brief Constructor specifying an external vector for storing the polygons
   */
  PolygonRefGenerator (db::Layout *layout, std::unordered_set<db::PolygonRef> &polyrefs)
    : PolygonSink (), mp_layout (layout), mp_polyrefs (&polyrefs)
  { }

  /**
   *  @brief Implementation of the PolygonSink interface
   */
  virtual void put (const db::Polygon &polygon)
  {
    tl::MutexLocker locker (&mp_layout->lock ());
    mp_polyrefs->insert (db::PolygonRef (polygon, mp_layout->shape_repository ()));
  }

private:
  db::Layout *mp_layout;
  std::unordered_set<db::PolygonRef> *mp_polyrefs;
};

class PolygonSplitter
  : public PolygonSink
{
public:
  PolygonSplitter (PolygonSink &sink, double max_area_ratio, size_t max_vertex_count)
    : mp_sink (&sink), m_max_area_ratio (max_area_ratio), m_max_vertex_count (max_vertex_count)
  {
    //  .. nothing yet ..
  }

  virtual void put (const db::Polygon &poly)
  {
    if ((m_max_vertex_count > 0 && poly.vertices () > m_max_vertex_count) || (m_max_area_ratio > 0.0 && poly.area_ratio () > m_max_area_ratio)) {

      std::vector <db::Polygon> split_polygons;
      db::split_polygon (poly, split_polygons);
      for (std::vector <db::Polygon>::const_iterator sp = split_polygons.begin (); sp != split_polygons.end (); ++sp) {
        put (*sp);
      }

    } else {
      mp_sink->put (poly);
    }
  }

  virtual void start () { mp_sink->start (); }
  virtual void flush () { mp_sink->flush (); }

private:
  PolygonSink *mp_sink;
  double m_max_area_ratio;
  size_t m_max_vertex_count;
};

}

// ---------------------------------------------------------------------------------------------

BoolAndOrNotLocalOperation::BoolAndOrNotLocalOperation (bool is_and, double max_area_ratio, size_t max_vertex_count)
  : m_is_and (is_and), m_max_area_ratio (max_area_ratio), m_max_vertex_count (max_vertex_count)
{
  //  .. nothing yet ..
}

LocalOperation::on_empty_intruder_mode
BoolAndOrNotLocalOperation::on_empty_intruder_hint () const
{
  return m_is_and ? LocalOperation::Drop : LocalOperation::Copy;
}

std::string
BoolAndOrNotLocalOperation::description () const
{
  return m_is_and ? tl::to_string (tr ("AND operation")) : tl::to_string (tr ("NOT operation"));
}

void
BoolAndOrNotLocalOperation::compute_local (db::Layout *layout, const ShapeInteractions &interactions, std::unordered_set<db::PolygonRef> &result) const
{
  db::EdgeProcessor ep;

  size_t p1 = 0, p2 = 1;

  std::set<db::PolygonRef> others;
  for (ShapeInteractions::iterator i = interactions.begin (); i != interactions.end (); ++i) {
    for (ShapeInteractions::iterator2 j = i->second.begin (); j != i->second.end (); ++j) {
      others.insert (interactions.shape (*j));
    }
  }

  for (ShapeInteractions::iterator i = interactions.begin (); i != interactions.end (); ++i) {

    const db::PolygonRef &subject = interactions.shape (i->first);
    if (others.find (subject) != others.end ()) {
      if (m_is_and) {
        result.insert (subject);
      }
    } else if (i->second.empty ()) {
      //  shortcut (not: keep, and: drop)
      if (! m_is_and) {
        result.insert (subject);
      }
    } else {
      for (db::PolygonRef::polygon_edge_iterator e = subject.begin_edge (); ! e.at_end(); ++e) {
        ep.insert (*e, p1);
      }
      p1 += 2;
    }

  }

  if (! others.empty () || p1 > 0) {

    for (std::set<db::PolygonRef>::const_iterator o = others.begin (); o != others.end (); ++o) {
      for (db::PolygonRef::polygon_edge_iterator e = o->begin_edge (); ! e.at_end(); ++e) {
        ep.insert (*e, p2);
      }
      p2 += 2;
    }

    db::BooleanOp op (m_is_and ? db::BooleanOp::And : db::BooleanOp::ANotB);
    db::PolygonRefGenerator pr (layout, result);
    db::PolygonSplitter splitter (pr, m_max_area_ratio, m_max_vertex_count);
    db::PolygonGenerator pg (splitter, true, true);
    ep.process (pg, op);

  }
}

// ---------------------------------------------------------------------------------------------

SelfOverlapMergeLocalOperation::SelfOverlapMergeLocalOperation (unsigned int wrap_count)
  : m_wrap_count (wrap_count)
{
  //  .. nothing yet ..
}

void SelfOverlapMergeLocalOperation::compute_local (db::Layout *layout, const ShapeInteractions &interactions, std::unordered_set<db::PolygonRef> &result) const
{
  if (m_wrap_count == 0) {
    return;
  }

  db::EdgeProcessor ep;

  size_t p1 = 0, p2 = 1;
  std::set<unsigned int> seen;

  for (ShapeInteractions::iterator i = interactions.begin (); i != interactions.end (); ++i) {

    if (seen.find (i->first) == seen.end ()) {
      seen.insert (i->first);
      const db::PolygonRef &subject = interactions.shape (i->first);
      for (db::PolygonRef::polygon_edge_iterator e = subject.begin_edge (); ! e.at_end(); ++e) {
        ep.insert (*e, p1);
      }
      p1 += 2;
    }

    for (db::ShapeInteractions::iterator2 o = i->second.begin (); o != i->second.end (); ++o) {
      //  don't take the same (really the same, not an identical one) shape twice - the interaction
      //  set does not take care to list just one copy of the same item on the intruder side.
      if (seen.find (*o) == seen.end ()) {
        seen.insert (*o);
        const db::PolygonRef &intruder = interactions.shape (*o);
        for (db::PolygonRef::polygon_edge_iterator e = intruder.begin_edge (); ! e.at_end(); ++e) {
          ep.insert (*e, p2);
        }
        p2 += 2;
      }
    }

  }

  db::MergeOp op (m_wrap_count - 1);
  db::PolygonRefGenerator pr (layout, result);
  db::PolygonGenerator pg (pr, true, true);
  ep.process (pg, op);
}

SelfOverlapMergeLocalOperation::on_empty_intruder_mode SelfOverlapMergeLocalOperation::on_empty_intruder_hint () const
{
  return m_wrap_count > 1 ? LocalOperation::Drop : LocalOperation::Copy;
}

std::string SelfOverlapMergeLocalOperation::description () const
{
  return tl::sprintf (tl::to_string (tr ("Self-overlap (wrap count %d)")), int (m_wrap_count));
}

}
