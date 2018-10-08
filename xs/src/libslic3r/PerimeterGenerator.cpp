#include "PerimeterGenerator.hpp"
#include "ClipperUtils.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "BridgeDetector.hpp"
#include "Geometry.hpp"
#include <cmath>
#include <cassert>
#include <vector>

#include "BoundingBox.hpp"
#include "ExPolygon.hpp"
#include "Geometry.hpp"
#include "Polygon.hpp"
#include "Line.hpp"
#include "ClipperUtils.hpp"
#include "SVG.hpp"
#include "polypartition.h"
#include "poly2tri/poly2tri.h"
#include <algorithm>
#include <cassert>
#include <list>

namespace Slic3r {

void PerimeterGenerator::process()
{
    // other perimeters
    this->_mm3_per_mm               = this->perimeter_flow.mm3_per_mm();
    coord_t perimeter_width         = this->perimeter_flow.scaled_width();
    coord_t perimeter_spacing       = this->perimeter_flow.scaled_spacing();
    
    // external perimeters
    this->_ext_mm3_per_mm           = this->ext_perimeter_flow.mm3_per_mm();
    coord_t ext_perimeter_width     = this->ext_perimeter_flow.scaled_width();
    coord_t ext_perimeter_spacing   = this->ext_perimeter_flow.scaled_spacing();
    coord_t ext_perimeter_spacing2  = this->ext_perimeter_flow.scaled_spacing(this->perimeter_flow);
    
    // overhang perimeters
    this->_mm3_per_mm_overhang      = this->overhang_flow.mm3_per_mm();
    
    // solid infill
    coord_t solid_infill_spacing    = this->solid_infill_flow.scaled_spacing();
    
    // Calculate the minimum required spacing between two adjacent traces.
    // This should be equal to the nominal flow spacing but we experiment
    // with some tolerance in order to avoid triggering medial axis when
    // some squishing might work. Loops are still spaced by the entire
    // flow spacing; this only applies to collapsing parts.
    // For ext_min_spacing we use the ext_perimeter_spacing calculated for two adjacent
    // external loops (which is the correct way) instead of using ext_perimeter_spacing2
    // which is the spacing between external and internal, which is not correct
    // and would make the collapsing (thus the details resolution) dependent on 
    // internal flow which is unrelated.
    coord_t min_spacing         = perimeter_spacing      * (1 - INSET_OVERLAP_TOLERANCE);
    coord_t ext_min_spacing     = ext_perimeter_spacing  * (1 - INSET_OVERLAP_TOLERANCE);
    
    // prepare grown lower layer slices for overhang detection
    if (this->lower_slices != NULL && this->config->overhangs) {
        // We consider overhang any part where the entire nozzle diameter is not supported by the
        // lower layer, so we take lower slices and offset them by half the nozzle diameter used 
        // in the current layer
        double nozzle_diameter = this->print_config->nozzle_diameter.get_at(this->config->perimeter_extruder-1);
        this->_lower_slices_p = offset(*this->lower_slices, float(scale_(+nozzle_diameter/2)));
    }
    
    // we need to process each island separately because we might have different
    // extra perimeters for each one
    int surface_idx = 0;
    for (const Surface &surface : this->slices->surfaces) {
        // detect how many perimeters must be generated for this island
        int        loop_number = this->config->perimeters + surface.extra_perimeters - 1;  // 0-indexed loops
        surface_idx++;

        if (this->config->only_one_perimeter_top && this->upper_slices == NULL){
            loop_number = 0;
        }
        
        ExPolygons gaps;
        //this var store infill surface removed from last to not add any more perimeters to it.
        ExPolygons stored;
        ExPolygons last        = union_ex(surface.expolygon.simplify_p(SCALED_RESOLUTION));
        if (loop_number >= 0) {
            // In case no perimeters are to be generated, loop_number will equal to -1.            
            std::vector<PerimeterGeneratorLoops> contours(loop_number+1);    // depth => loops
            std::vector<PerimeterGeneratorLoops> holes(loop_number+1);       // depth => loops
            ThickPolylines thin_walls;
            // we loop one time more than needed in order to find gaps after the last perimeter was applied
            for (int i = 0;; ++ i) {  // outer loop is 0

                //store surface for bridge infill to avoid unsupported perimeters (but the first one, this one is always good)
                if (this->config->no_perimeter_unsupported && i == this->config->min_perimeter_unsupported
                    && this->lower_slices != NULL && !this->lower_slices->expolygons.empty()) {

                    //compute our unsupported surface
                    ExPolygons unsupported = diff_ex(last, this->lower_slices->expolygons, true);
                    if (!unsupported.empty()) {
                        //remove small overhangs
                        ExPolygons unsupported_filtered = offset2_ex(unsupported, -(float)(perimeter_spacing), (float)(perimeter_spacing));
                        if (!unsupported_filtered.empty()) {
                            //to_draw.insert(to_draw.end(), last.begin(), last.end());
                            //extract only the useful part of the lower layer. The safety offset is really needed here.
                            ExPolygons support = diff_ex(last, unsupported, true);
                            if (this->config->noperi_bridge_only && !unsupported.empty()) {
                                //only consider the part that can be bridged (really, by the bridge algorithm)
                                //first, separate into islands (ie, each ExPlolygon)
                                int numploy = 0;
                                //only consider the bottom layer that intersect unsupported, to be sure it's only on our island.
                                ExPolygonCollection lower_island(support);
                                BridgeDetector detector(unsupported_filtered,
                                    lower_island,
                                    perimeter_spacing);
                                if (detector.detect_angle(Geometry::deg2rad(this->config->bridge_angle.value))) {
                                    ExPolygons bridgeable = union_ex(detector.coverage(-1, true));
                                    if (!bridgeable.empty()) {
                                        //simplify to avoid most of artefacts from printing lines.
                                        ExPolygons bridgeable_simplified;
                                        for (ExPolygon &poly : bridgeable) {
                                            poly.simplify(perimeter_spacing/2, &bridgeable_simplified);
                                        }
                                        //offset by perimeter spacing because the simplify may have reduced it a bit.
                                        //it's not dangerous as it will be intersected by 'unsupported' later
                                        //FIXME: add overlap in this->fill_surfaces->append
                                        // add overlap (perimeter_spacing/4 was good in test, ie 25%)
                                        coord_t overlap = scale_(this->config->get_abs_value("infill_overlap", unscale(perimeter_spacing)));
                                        unsupported_filtered = intersection_ex(unsupported_filtered, offset_ex(bridgeable_simplified, overlap));
                                    } else {
                                        unsupported_filtered.clear();
                                    }
                                } else {
                                    unsupported_filtered.clear();
                                }
                            } else {
                                //only consider the part that can be 'bridged' (inside the convex hull)
                                // it's not as precise as the bridge detector, but it's better than nothing, and quicker.
                            ExPolygonCollection coll_last(support);
                            ExPolygon hull;
                            hull.contour = coll_last.convex_hull();
                                unsupported_filtered = intersection_ex(unsupported_filtered, ExPolygons() = { hull });
                            }
                            if (!unsupported_filtered.empty()) {
                                //and we want at least 1 perimeter of overlap
                                ExPolygons bridge = unsupported_filtered;
                                unsupported_filtered = intersection_ex(offset_ex(unsupported_filtered, (float)(perimeter_spacing)), last);
                                // remove from the bridge & support the small imperfections of the union
                                ExPolygons bridge_and_support = offset2_ex(union_ex(bridge, support, true), perimeter_spacing/2, -perimeter_spacing/2);
                                // make him flush with perimeter area
                                unsupported_filtered = intersection_ex(offset_ex(unsupported_filtered, (float)(perimeter_spacing / 2)), bridge_and_support);
                                
                                //add this directly to the infill list.
                                // this will avoid to throw wrong offsets into a good polygons
                                this->fill_surfaces->append(
                                    unsupported_filtered,
                                    stInternal);
                                
                                // store the results
                                last = diff_ex(last, unsupported_filtered, true);
                                //remove "thin air" polygons
                                for (int i = 0; i < last.size();i++) {
                                    if (intersection_ex(support, ExPolygons() = { last[i] }).empty()) {
                                        this->fill_surfaces->append(
                                            ExPolygons() = { last[i] },
                                            stInternal);
                                        last.erase(last.begin() + i);
                                        i--;
                                    }
                                }
                            }
                        }
                    }
                }

                // We can add more perimeters if there are uncovered overhangs
                // improvement for future: find a way to add perimeters only where it's needed.
                // It's hard to do, so here is a simple version.
                bool has_overhang = false;
                if (this->config->extra_perimeters /*&& i > loop_number*/ && !last.empty()
                    && this->lower_slices != NULL && !this->lower_slices->expolygons.empty()){
                    //split the polygons with bottom/notbottom
                    ExPolygons unsupported = diff_ex(last, this->lower_slices->expolygons, true);
                    if (!unsupported.empty()) {
                        //only consider overhangs and let bridges alone
                        if (true) {
                            //only consider the part that can be bridged (really, by the bridge algorithm)
                            //first, separate into islands (ie, each ExPlolygon)
                            int numploy = 0;
                            //only consider the bottom layer that intersect unsupported, to be sure it's only on our island.
                            ExPolygonCollection lower_island(diff_ex(last, unsupported, true));
                            BridgeDetector detector(unsupported,
                                lower_island,
                                perimeter_spacing);
                            if (detector.detect_angle(Geometry::deg2rad(this->config->bridge_angle.value))) {
                                ExPolygons bridgeable = union_ex(detector.coverage(-1, true));
                                if (!bridgeable.empty()) {
                                    //simplify to avoid most of artefacts from printing lines.
                                    ExPolygons bridgeable_simplified;
                                    for (ExPolygon &poly : bridgeable) {
                                        poly.simplify(perimeter_spacing / 2, &bridgeable_simplified);
                                    }
                                    
                                    if (!bridgeable_simplified.empty())
                                        bridgeable_simplified = offset_ex(bridgeable_simplified, perimeter_spacing/1.9);
                                    if (!bridgeable_simplified.empty()) {
                                        //offset by perimeter spacing because the simplify may have reduced it a bit.
                                        unsupported = diff_ex(unsupported, bridgeable_simplified, true);
                                    }
                                } 
                            }
                        } else {
                            ExPolygonCollection coll_last(intersection_ex(last, offset_ex(this->lower_slices->expolygons, -perimeter_spacing / 2)));
                            ExPolygon hull;
                            hull.contour = coll_last.convex_hull();
                            unsupported = diff_ex(offset_ex(unsupported, perimeter_spacing), ExPolygons() = { hull });
                        }
                        if (!unsupported.empty()) {
                            //add fake perimeters here
                            has_overhang = true;
                        }
                    }
                }

                // Calculate next onion shell of perimeters.
                //this variable stored the nexyt onion
                ExPolygons next_onion;
                if (i == 0) {
                    // compute next onion, without taking care of thin_walls : destroy too thin areas.
                    if (!this->config->thin_walls)
                        next_onion = offset_ex(last, -(float)(ext_perimeter_width / 2));


                    // look for thin walls
                    if (this->config->thin_walls) {
                        // the minimum thickness of a single loop is:
                        // ext_width/2 + ext_spacing/2 + spacing/2 + width/2

                        next_onion = offset2_ex(
                            last,
                            -(float)(ext_perimeter_width / 2 + ext_min_spacing / 2 - 1),
                            +(float)(ext_min_spacing / 2 - 1));

                        // detect edge case where a curve can be split in multiple small chunks.
                        ExPolygons no_thin_onion = offset_ex(last, -(float)(ext_perimeter_width / 2));
                        if (no_thin_onion.size()>0 && next_onion.size() > 3 * no_thin_onion.size()) {
                            //use a sightly smaller spacing to try to drastically improve the split
                            ExPolygons next_onion_secondTry = offset2_ex(
                                last,
                                -(float)(ext_perimeter_width / 2 + ext_min_spacing / 2.5 - 1),
                                +(float)(ext_min_spacing / 2.5 - 1));
                            if (abs(((int32_t)next_onion.size()) - ((int32_t)no_thin_onion.size())) > 
                                2*abs(((int32_t)next_onion_secondTry.size()) - ((int32_t)no_thin_onion.size()))) {
                                next_onion = next_onion_secondTry;
                            }
                        }

                        // the following offset2 ensures almost nothing in @thin_walls is narrower than $min_width
                        // (actually, something larger than that still may exist due to mitering or other causes)
                        coord_t min_width = (coord_t)scale_(this->ext_perimeter_flow.nozzle_diameter / 3);
                        
                        ExPolygons no_thin_zone = offset_ex(next_onion, (float)(ext_perimeter_width / 2), jtSquare);
                        // medial axis requires non-overlapping geometry
                        ExPolygons thin_zones = diff_ex(last, no_thin_zone, true);
                        //don't use offset2_ex, because we don't want to merge the zones that have been separated.
                        ExPolygons expp = offset_ex(thin_zones, (float)(-min_width / 2));
                        //we push the bits removed and put them into what we will use as our anchor
                        if (expp.size() > 0) {
                            no_thin_zone = diff_ex(last, offset_ex(expp, (float)(min_width / 2)), true);
                        }
                        // compute a bit of overlap to anchor thin walls inside the print.
                        for (ExPolygon &ex : expp) {
                            //growing back the polygon
                            //a very little bit of overlap can be created here with other thin polygons, but it's more useful than worisome.
                            ex.remove_point_too_near(SCALED_RESOLUTION);
                            ExPolygons ex_bigger = offset_ex(ex, (float)(min_width / 2));
                            if (ex_bigger.size() != 1) continue; // impossible error, growing a single polygon can't create multiple or 0.
                            ExPolygons anchor = intersection_ex(offset_ex(ex, (float)(min_width / 2) + 
                                (float)(ext_perimeter_width / 2), jtSquare), no_thin_zone, true);
                            ExPolygons bounds = union_ex(ex_bigger, anchor, true);
                            for (ExPolygon &bound : bounds) {
                                if (!intersection_ex(ex_bigger[0], bound).empty()) {
                                    //be sure it's not too small to extrude reliably
                                    if (ex_bigger[0].area() > min_width*(ext_perimeter_width + ext_perimeter_spacing2)) {
                                        // the maximum thickness of our thin wall area is equal to the minimum thickness of a single loop
                                        ex_bigger[0].medial_axis(bound, ext_perimeter_width + ext_perimeter_spacing2, min_width,
                                            &thin_walls, this->layer_height);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    //FIXME Is this offset correct if the line width of the inner perimeters differs
                    // from the line width of the infill?
                    coord_t distance = (i == 1) ? ext_perimeter_spacing2 : perimeter_spacing;
                    if (this->config->thin_walls){
                        // This path will ensure, that the perimeters do not overfill, as in 
                        // prusa3d/Slic3r GH #32, but with the cost of rounding the perimeters
                        // excessively, creating gaps, which then need to be filled in by the not very 
                        // reliable gap fill algorithm.
                        // Also the offset2(perimeter, -x, x) may sometimes lead to a perimeter, which is larger than
                        // the original.
                        next_onion = offset2_ex(last,
                            -(float)(distance + min_spacing / 2 - 1),
                            +(float)(min_spacing / 2 - 1));
                    } else {
                        // If "detect thin walls" is not enabled, this paths will be entered, which 
                        // leads to overflows, as in prusa3d/Slic3r GH #32
                        next_onion = offset_ex(last, -(float)(distance));
                    }
                    // look for gaps
                    if (this->config->gap_fill_speed.value > 0 && this->config->fill_density.value > 0)
                        // not using safety offset here would "detect" very narrow gaps
                        // (but still long enough to escape the area threshold) that gap fill
                        // won't be able to fill but we'd still remove from infill area
                        append(gaps, diff_ex(
                            offset(last, -0.5f*distance),
                            offset(next_onion, 0.5f * distance + 10)));  // safety offset
                }

                if (next_onion.empty()) {
                    // Store the number of loops actually generated.
                    loop_number = i - 1;
                    // No region left to be filled in.
                    last.clear();
                    break;
                } else if (i > loop_number) {
                    if (has_overhang) {
                        loop_number++;
                        contours.emplace_back();
                        holes.emplace_back();
                    } else {
                        // If i > loop_number, we were looking just for gaps.
                        break;
                    }
                }

                for (const ExPolygon &expolygon : next_onion) {
                    contours[i].emplace_back(PerimeterGeneratorLoop(expolygon.contour, i, true, has_overhang));
                    if (! expolygon.holes.empty()) {
                        holes[i].reserve(holes[i].size() + expolygon.holes.size());
                        for (const Polygon &hole : expolygon.holes)
                            holes[i].emplace_back(PerimeterGeneratorLoop(hole, i, false, has_overhang));
                    }
                }
                last = std::move(next_onion);
                    
                //store surface for top infill if only_one_perimeter_top
                if(i==0 && config->only_one_perimeter_top && this->upper_slices != NULL){
                    //split the polygons with top/not_top
                    ExPolygons upper_polygons(this->upper_slices->expolygons);
                    ExPolygons top_polygons = diff_ex(last, (upper_polygons), true);
                    ExPolygons inner_polygons = diff_ex(last, top_polygons, true);
                    // increase a bit the inner space to fill the frontier between last and stored.
                    stored = union_ex(stored, intersection_ex(offset_ex(top_polygons, (float)(perimeter_spacing / 2)), last));
                    last = intersection_ex(offset_ex(inner_polygons, (float)(perimeter_spacing / 2)), last);
                }

                

            }
            
            // re-add stored polygons
            last = union_ex(last, stored);

            // nest loops: holes first
            for (int d = 0; d <= loop_number; ++d) {
                PerimeterGeneratorLoops &holes_d = holes[d];
                // loop through all holes having depth == d
                for (int i = 0; i < (int)holes_d.size(); ++i) {
                    const PerimeterGeneratorLoop &loop = holes_d[i];
                    // find the hole loop that contains this one, if any
                    for (int t = d+1; t <= loop_number; ++t) {
                        for (int j = 0; j < (int)holes[t].size(); ++j) {
                            PerimeterGeneratorLoop &candidate_parent = holes[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                holes_d.erase(holes_d.begin() + i);
                                --i;
                                goto NEXT_LOOP;
                            }
                        }
                    }
                    // if no hole contains this hole, find the contour loop that contains it
                    for (int t = loop_number; t >= 0; --t) {
                        for (int j = 0; j < (int)contours[t].size(); ++j) {
                            PerimeterGeneratorLoop &candidate_parent = contours[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                holes_d.erase(holes_d.begin() + i);
                                --i;
                                goto NEXT_LOOP;
                            }
                        }
                    }
                    NEXT_LOOP: ;
                }
            }
            // nest contour loops
            for (int d = loop_number; d >= 1; --d) {
                PerimeterGeneratorLoops &contours_d = contours[d];
                // loop through all contours having depth == d
                for (int i = 0; i < (int)contours_d.size(); ++i) {
                    const PerimeterGeneratorLoop &loop = contours_d[i];
                    // find the contour loop that contains it
                    for (int t = d-1; t >= 0; --t) {
                        for (int j = 0; j < contours[t].size(); ++j) {
                            PerimeterGeneratorLoop &candidate_parent = contours[t][j];
                            if (candidate_parent.polygon.contains(loop.polygon.first_point())) {
                                candidate_parent.children.push_back(loop);
                                contours_d.erase(contours_d.begin() + i);
                                --i;
                                goto NEXT_CONTOUR;
                            }
                        }
                    }
                    NEXT_CONTOUR: ;
                }
            }
            ExtrusionEntityCollection entities;
            //onlyone_perimter = >fusion all perimeterLoops
            {
                for (PerimeterGeneratorLoop &loop : contours.front()) {
                    std::cout << "_traverse_and_join_loops call\n";
                    ExtrusionLoop extr_loop = this->_traverse_and_join_loops(loop, loop.polygon.points.front());
                    std::cout << "insert last point:\n";
                    extr_loop.paths.back().polyline.points.push_back(extr_loop.paths.front().polyline.points.front());
                    std::cout << "inserted last point:";
                    entities.append(extr_loop);

                    //loop.polygon.points = polyline.points;

                        std::cout << "looppath:";
                        for (ExtrusionPath &path : extr_loop.paths) {
                            std::cout << " !";
                            for (Point &p : path.polyline.points) {
                                std::cout << " " << unscale(p.x) << ":" << unscale(p.y);
                            }
                        }
                        std::cout << "\n";
                    

                    std::cout << " -- check1\n";
                    extr_loop.check();
                    SVG svg("polygon.svg");
                    svg.draw(extr_loop.polygon());
                    svg.Close();
                    SVG svg2("polyline.svg");
                    /*for (ExtrusionEntity *path : entities.flatten().entities) {
                        svg2.draw(path->as_polyline());
                    }*/
                    svg.draw(extr_loop.as_polyline());
                    svg2.Close();
                    
                }
            }
            std::cout << " == check2\n";
            entities.check();

            // at this point, all loops should be in contours[0]
            /*ExtrusionEntityCollection entities_test = this->_traverse_loops(contours.front(), thin_walls);
            ExtrusionLoop *firstPath = dynamic_cast<ExtrusionLoop*>(entities_test.entities.front());
            if (firstPath) {
                std::cout << "firstLoop / " << entities_test.entities.size()<<":";
                for (ExtrusionPath &path : firstPath->paths) {
                    std::cout << " !";
                    for (Point &p : path.polyline.points) {
                        std::cout << " "<<unscale(p.x)<<":"<<unscale(p.y);
                    }
                }
            }*/

            // if brim will be printed, reverse the order of perimeters so that
            // we continue inwards after having finished the brim
            // TODO: add test for perimeter order
            if (this->config->external_perimeters_first || 
                (this->layer_id == 0 && this->print_config->brim_width.value > 0))
                    entities.reverse();
            // append perimeters for this slice as a collection
            if (!entities.empty())
                this->loops->append(entities);
        } // for each loop of an island

        // fill gaps
        if (!gaps.empty()) {
            // collapse 
            double min = 0.2 * perimeter_width * (1 - INSET_OVERLAP_TOLERANCE);
            double max = 2. * perimeter_spacing;
            ExPolygons gaps_ex = diff_ex(
                offset2_ex(gaps, -min/2, +min/2),
                offset2_ex(gaps, -max/2, +max/2),
                true);
            ThickPolylines polylines;
            for (const ExPolygon &ex : gaps_ex) {
                //remove too small gaps that are too hard to fill.
                //ie one that are smaller than an extrusion with width of min and a length of max.
                if (ex.area() > min*max) {
                    ex.medial_axis(ex, max, min, &polylines, this->layer_height);
                }
            }
            if (!polylines.empty()) {
                ExtrusionEntityCollection gap_fill = this->_variable_width(polylines, 
                    erGapFill, this->solid_infill_flow);
                this->gap_fill->append(gap_fill.entities);
                /*  Make sure we don't infill narrow parts that are already gap-filled
                    (we only consider this surface's gaps to reduce the diff() complexity).
                    Growing actual extrusions ensures that gaps not filled by medial axis
                    are not subtracted from fill surfaces (they might be too short gaps
                    that medial axis skips but infill might join with other infill regions
                    and use zigzag).  */
                //FIXME Vojtech: This grows by a rounded extrusion width, not by line spacing,
                // therefore it may cover the area, but no the volume.
                last = diff_ex(to_polygons(last), gap_fill.polygons_covered_by_width(10.f));
            }
        }

        // create one more offset to be used as boundary for fill
        // we offset by half the perimeter spacing (to get to the actual infill boundary)
        // and then we offset back and forth by half the infill spacing to only consider the
        // non-collapsing regions
        coord_t inset = 
            (loop_number < 0) ? 0 :
            (loop_number == 0) ?
            // one loop
                ext_perimeter_spacing / 2 :
                // two or more loops?
                perimeter_spacing / 2;
        // only apply infill overlap if we actually have one perimeter
        coord_t overlap = 0;
        if (inset > 0) {
            overlap = scale_(this->config->get_abs_value("infill_overlap", unscale(inset + solid_infill_spacing / 2)));
        }
        // simplify infill contours according to resolution
        Polygons pp;
        for (ExPolygon &ex : last)
            ex.simplify_p(SCALED_RESOLUTION, &pp);
        // collapse too narrow infill areas
        coord_t min_perimeter_infill_spacing = solid_infill_spacing * (1. - INSET_OVERLAP_TOLERANCE);
        // append infill areas to fill_surfaces
        //auto it_surf = this->fill_surfaces->surfaces.end();
        this->fill_surfaces->append(
            offset2_ex(
                union_ex(pp),
                -inset - min_perimeter_infill_spacing / 2 + overlap,
                min_perimeter_infill_spacing / 2),
                stInternal);
        if (overlap != 0) {
            ExPolygons polyWithoutOverlap = offset2_ex(
                union_ex(pp),
                -inset - min_perimeter_infill_spacing / 2,
                min_perimeter_infill_spacing / 2);
            this->fill_no_overlap.insert(this->fill_no_overlap.end(), polyWithoutOverlap.begin(), polyWithoutOverlap.end());
        }
    } // for each island
}


ExtrusionEntityCollection PerimeterGenerator::_traverse_loops(
    const PerimeterGeneratorLoops &loops, ThickPolylines &thin_walls) const
{
    // loops is an arrayref of ::Loop objects
    // turn each one into an ExtrusionLoop object
    ExtrusionEntityCollection coll;
    for (PerimeterGeneratorLoops::const_iterator loop = loops.begin();
        loop != loops.end(); ++loop) {
        bool is_external = loop->is_external();
        
        ExtrusionRole role;
        ExtrusionLoopRole loop_role;
        role = is_external ? erExternalPerimeter : erPerimeter;
        if (loop->is_internal_contour()) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = elrContourInternalPerimeter;
        } else {
            loop_role = elrDefault;
        }
        
        // detect overhanging/bridging perimeters
        ExtrusionPaths paths;
        if (this->config->overhangs && this->layer_id > 0
            && !(this->object_config->support_material && this->object_config->support_material_contact_distance.value == 0)) {
            // get non-overhang paths by intersecting this loop with the grown lower slices
            extrusion_paths_append(
                paths,
                intersection_pl(loop->polygon, this->_lower_slices_p),
                role,
                is_external ? this->_ext_mm3_per_mm           : this->_mm3_per_mm,
                is_external ? this->ext_perimeter_flow.width  : this->perimeter_flow.width,
                this->layer_height);
            
            // get overhang paths by checking what parts of this loop fall 
            // outside the grown lower slices (thus where the distance between
            // the loop centerline and original lower slices is >= half nozzle diameter
            extrusion_paths_append(
                paths,
                diff_pl(loop->polygon, this->_lower_slices_p),
                erOverhangPerimeter,
                this->_mm3_per_mm_overhang,
                this->overhang_flow.width,
                this->overhang_flow.height);
            
            // reapply the nearest point search for starting point
            // We allow polyline reversal because Clipper may have randomly
            // reversed polylines during clipping.
            paths = (ExtrusionPaths)ExtrusionEntityCollection(paths).chained_path();
        } else {
            ExtrusionPath path(role);
            path.polyline   = loop->polygon.split_at_first_point();
            path.mm3_per_mm = is_external ? this->_ext_mm3_per_mm           : this->_mm3_per_mm;
            path.width      = is_external ? this->ext_perimeter_flow.width  : this->perimeter_flow.width;
            path.height     = this->layer_height;
            paths.push_back(path);
        }
        
        coll.append(ExtrusionLoop(paths, loop_role));
    }
    
    // append thin walls to the nearest-neighbor search (only for first iteration)
    if (!thin_walls.empty()) {
        ExtrusionEntityCollection tw = this->_variable_width
            (thin_walls, erExternalPerimeter, this->ext_perimeter_flow);
        
        coll.append(tw.entities);
        thin_walls.clear();
    }
    
    // sort entities into a new collection using a nearest-neighbor search,
    // preserving the original indices which are useful for detecting thin walls
    ExtrusionEntityCollection sorted_coll;
    coll.chained_path(&sorted_coll, false, erMixed, &sorted_coll.orig_indices);
    
    // traverse children and build the final collection
    ExtrusionEntityCollection entities;
    for (std::vector<size_t>::const_iterator idx = sorted_coll.orig_indices.begin();
        idx != sorted_coll.orig_indices.end();
        ++idx) {
        
        if (*idx >= loops.size()) {
            // this is a thin wall
            // let's get it from the sorted collection as it might have been reversed
            size_t i = idx - sorted_coll.orig_indices.begin();
            entities.append(*sorted_coll.entities[i]);
        } else {
            const PerimeterGeneratorLoop &loop = loops[*idx];
            ExtrusionLoop eloop = *dynamic_cast<ExtrusionLoop*>(coll.entities[*idx]);
            
            ExtrusionEntityCollection children = this->_traverse_loops(loop.children, thin_walls);
            if (loop.is_contour) {
                if (loop.is_overhang && this->layer_id % 2 == 1)
                    eloop.make_clockwise();
                else
                    eloop.make_counter_clockwise();
                entities.append(children.entities);
                entities.append(eloop);
            } else {
                eloop.make_clockwise();
                entities.append(eloop);
                entities.append(children.entities);
            }
        }
    }
    return entities;
}

int id = 0;
ExtrusionLoop
PerimeterGenerator::_traverse_and_join_loops(const PerimeterGeneratorLoop &loop, Point entryPoint, bool has_to_reverse) const
{
    const int my_id = id++;
    std::cout << my_id << "_traverse_and_join_loops\n";
    const coord_t dist_cut = this->perimeter_flow.scaled_width();
    //std::cout << "dist_cut=" << dist_cut<<"\n";
    //TODO change this->perimeter_flow.scaled_width() if it's the first one!
    //myPolyline
    Polyline initialPolyline = loop.polygon.split_at_vertex(entryPoint);
    if (has_to_reverse && loop.is_contour || !loop.is_contour && !has_to_reverse) initialPolyline.reverse();
    std::cout << my_id << "myPolyline.pointsize = " << initialPolyline.points.size() << "\n";
    initialPolyline.clip_end(dist_cut);
    //std::cout << "clip_end ok " << initialPolyline.points.size() << "\n";

    std::vector<PerimeterPolylineNode> myPolylines;


    ExtrusionLoop svg_out(elrDefault);
    //overhang / notoverhang
    {
        bool is_external = loop.is_external();

        ExtrusionRole role;
        ExtrusionLoopRole loop_role;
        role = is_external ? erExternalPerimeter : erPerimeter;
        if (loop.is_internal_contour()) {
            // Note that we set loop role to ContourInternalPerimeter
            // also when loop is both internal and external (i.e.
            // there's only one contour loop).
            loop_role = elrContourInternalPerimeter;
        } else {
            loop_role = elrDefault;
        }

        // detect overhanging/bridging perimeters
        if (this->config->overhangs && this->layer_id > 0
            && !(this->object_config->support_material && this->object_config->support_material_contact_distance.value == 0)) {
            ExtrusionPaths paths;
            // get non-overhang paths by intersecting this loop with the grown lower slices
            extrusion_paths_append(
                paths,
                intersection_pl(initialPolyline, this->_lower_slices_p),
                role,
                is_external ? this->_ext_mm3_per_mm : this->_mm3_per_mm,
                is_external ? this->ext_perimeter_flow.width : this->perimeter_flow.width,
                this->layer_height);

            // get overhang paths by checking what parts of this loop fall 
            // outside the grown lower slices (thus where the distance between
            // the loop centerline and original lower slices is >= half nozzle diameter
            extrusion_paths_append(
                paths,
                diff_pl(initialPolyline, this->_lower_slices_p),
                erOverhangPerimeter,
                this->_mm3_per_mm_overhang,
                this->overhang_flow.width,
                this->overhang_flow.height);

            // reapply the nearest point search for starting point
            // We allow polyline reversal because Clipper may have randomly
            // reversed polylines during clipping.
            paths = (ExtrusionPaths)ExtrusionEntityCollection(paths).chained_path();

            //TODO: add a point a bit before last point to allow to use the last point as "return point"
            for (ExtrusionPath path : paths) {
                myPolylines.emplace_back(ExtrusionLoop(elrDefault), path);
                svg_out.paths.push_back(path);
            }

        } else {
            ExtrusionPath path(role);
            path.polyline = initialPolyline;
            path.mm3_per_mm = is_external ? this->_ext_mm3_per_mm : this->_mm3_per_mm;
            path.width = is_external ? this->ext_perimeter_flow.width : this->perimeter_flow.width;
            path.height = (float)(this->layer_height);
            myPolylines.emplace_back(ExtrusionLoop(elrDefault), path);
            svg_out.paths.push_back(path);
        }

    }
    {
        stringstream stname;
        stname << my_id << "_before_poly.svg";
        SVG svg(stname.str());
        svg.draw(svg_out.polygon());
        svg.Close();
    }

    //TODO : create poitns at middle of lines ?

    //Polylines myPolylines = { myPolyline };
    //iterate on each point ot find the best place to go into the child
    int child_idx = 0;
    for (const PerimeterGeneratorLoop &child : loop.children) {
        std::cout << my_id << "check child " << ++child_idx << "\n";
        coord_t smallest_dist = (coord_t)(dist_cut * 4.1);
        size_t smallest_idx = -1;
        size_t my_smallest_idx = -1;
        size_t my_polyline_idx = -1;
        std::cout << my_id << "nbMyPolys = " << myPolylines.size() << "\n";
        for (size_t idx_poly = 0; idx_poly < myPolylines.size(); idx_poly++) {
            std::cout << my_id << "check poly " << idx_poly << ", nbpoints = " << myPolylines[idx_poly].me.polyline.points.size() << "\n";
            if (myPolylines[idx_poly].me.length() + SCALED_EPSILON < dist_cut) continue;
            for (size_t idx_point = 0; idx_point < myPolylines[idx_poly].me.polyline.points.size() - 1; idx_point++) {
                //std::cout << "check point " << idx_point << "\n";
                const Point &p = myPolylines[idx_poly].me.polyline.points[idx_point];
                //TODO: get the best point in line, not in points
                const size_t nearest_idx = child.polygon.closest_point_index(p);
                const coord_t dist = (coord_t)child.polygon[nearest_idx].distance_to(p);
                if (dist < smallest_dist) {
                    //test if there are enough space
                    {
                        Polyline new_polyline = myPolylines[idx_poly].me.polyline;
                        new_polyline.points.erase(new_polyline.points.begin(), new_polyline.points.begin() + idx_point);
                        //not enough space => continue with next polyline
                        if (new_polyline.length()+SCALED_EPSILON < dist_cut) break;
                    }
                    //std::cout << "good point\n";
                    //ok, copy the idx
                    smallest_dist = dist;
                    smallest_idx = nearest_idx;
                    my_smallest_idx = idx_point;
                    my_polyline_idx = idx_poly;
                }
            }
        }
        std::cout << my_id << "call child: " << my_polyline_idx << ", " << my_smallest_idx << " => " << smallest_idx << "\n";
        if (smallest_idx == (size_t)-1) {
            std::cout << "ERROR: _traverse_and_join_loops: can't find a point near enough! => don't extrude this perimeter\n";
            //return ExtrusionEntityCollection();
            continue;
        } else {
            //create new node with recursive ask for the inner perimeter & COPY of the points, ready to be cut
            myPolylines.insert(myPolylines.begin() + my_polyline_idx + 1, 
                PerimeterPolylineNode(_traverse_and_join_loops(child, child.polygon.points[smallest_idx], !has_to_reverse), 
                    ExtrusionPath(myPolylines[my_polyline_idx].me)));
            PerimeterPolylineNode &new_node = myPolylines[my_polyline_idx+1];

            //cut our polyline
            std::cout << my_id << "new sizes (@" << my_smallest_idx<<") = " << myPolylines[my_polyline_idx].me.polyline.points.size() << " == " << new_node.me.polyline.points.size() << " ( > " << (my_smallest_idx + 1) << ")";
            //separate them
            new_node.me.polyline.points.erase(new_node.me.polyline.points.begin(), new_node.me.polyline.points.begin() + my_smallest_idx);
            Points &my_polyline_points = myPolylines[my_polyline_idx].me.polyline.points;
            my_polyline_points.erase(my_polyline_points.begin() + my_smallest_idx + 1, my_polyline_points.end());
            std::cout << " == " << myPolylines[my_polyline_idx].me.polyline.points.size() << " + " << new_node.me.polyline.points.size() << "\n";
            //trim the end/begining
            new_node.me.polyline.clip_start(dist_cut - SCALED_EPSILON);


            std::cout << my_id << "trimmed=>" << new_node.me.polyline.points.size() << "\n";
        }
    }
    std::cout << my_id << "check child end ""\n";
    std::cout << my_id << "nbMyPolysEnd = " << myPolylines.size() << "\n";
    for (size_t idx_poly = 0; idx_poly < myPolylines.size(); idx_poly++) {
        std::cout << my_id << "checkend poly " << idx_poly << ", nbpoints = " << myPolylines[idx_poly].me.polyline.points.size() 
            << " " << &(myPolylines[idx_poly]) << "\n";
    }


    std::cout << my_id << "create my complete polyline with childs. " <<" \n";
    //create the polyline
    ExtrusionLoop finalPolyline(elrContourInternalPerimeter);
    std::cout << my_id << "finalPolyline.points created, size = 0 =?= " << finalPolyline.paths.size() << " !\n";
    
    PerimeterPolylineNode &first_node = myPolylines.front();
    //first one is always an extrusionpath without any loop.
    if (!first_node.to_extrude_before.paths.empty()) std::cout << "error, not empty first Node\n";
    finalPolyline.paths.push_back(first_node.me);
    std::cout << my_id << "append, first cpath size=" << first_node.me.polyline.points.size() << " \n";
    Point last_point = first_node.me.polyline.points.back();
    for (size_t idx_node = 1; idx_node < myPolylines.size(); idx_node++) {
        PerimeterPolylineNode &current_node = myPolylines.front();

        std::cout << my_id << "insert inner node " << current_node.to_extrude_before.paths.empty() << " \n";
        if (!current_node.to_extrude_before.paths.empty()) {
            //first one is always an extrusionpath.
            ExtrusionPath &firstPath = current_node.to_extrude_before.paths.front();
            firstPath.polyline.points.insert(firstPath.polyline.points.begin(), last_point);
            finalPolyline.paths.insert(finalPolyline.paths.end(), 
                current_node.to_extrude_before.paths.begin(), current_node.to_extrude_before.paths.end());
            std::cout << my_id << "insert child of size = " << current_node.to_extrude_before.paths.size() << "\n";
            //last one is always an extrusionpath. (see lines a bit below)
            last_point = current_node.to_extrude_before.paths.back().polyline.points.back();
        }
        std::cout << my_id << "finalPolyline.points size = " << finalPolyline.paths.size() 
            << " and adding " << current_node.me.polyline.points.size() << "\n";
        if (!current_node.me.polyline.points.empty()) {
            current_node.me.polyline.points.insert(current_node.me.polyline.points.begin(), last_point);
            finalPolyline.paths.push_back(current_node.me);
            last_point = current_node.me.polyline.points.back();
        } else {
            std::cout << my_id << "ERROR, empty cpath\n";
        }

    }

    std::cout << my_id << "RETURN " << finalPolyline.paths.size() << " \n";
    {
        stringstream stname;
        stname << my_id << "_poly.svg";
        SVG svg(stname.str());
        svg.draw(finalPolyline.polygon());
        svg.Close();
    }
    finalPolyline.check();
    return finalPolyline;
}


ExtrusionEntityCollection PerimeterGenerator::_variable_width(const ThickPolylines &polylines, ExtrusionRole role, Flow flow) const
{
    // this value determines granularity of adaptive width, as G-code does not allow
    // variable extrusion within a single move; this value shall only affect the amount
    // of segments, and any pruning shall be performed before we apply this tolerance
    const double tolerance = scale_(0.05);
    
    int id_line = 0;
    ExtrusionEntityCollection coll;
    for (const ThickPolyline &p : polylines) {
        id_line++;
        ExtrusionPaths paths;
        ExtrusionPath path(role);
        ThickLines lines = p.thicklines();
        
        for (int i = 0; i < (int)lines.size(); ++i) {
            const ThickLine& line = lines[i];
            
            const coordf_t line_len = line.length();
            if (line_len < SCALED_EPSILON) continue;
            
            double thickness_delta = fabs(line.a_width - line.b_width);
            if (thickness_delta > tolerance) {
                const unsigned short segments = ceil(thickness_delta / tolerance);
                const coordf_t seg_len = line_len / segments;
                Points pp;
                std::vector<coordf_t> width;
                {
                    pp.push_back(line.a);
                    width.push_back(line.a_width);
                    for (size_t j = 1; j < segments; ++j) {
                        pp.push_back(line.point_at(j*seg_len));
                        
                        coordf_t w = line.a_width + (j*seg_len) * (line.b_width-line.a_width) / line_len;
                        width.push_back(w);
                        width.push_back(w);
                    }
                    pp.push_back(line.b);
                    width.push_back(line.b_width);
                    
                    assert(pp.size() == segments + 1);
                    assert(width.size() == segments*2);
                }
                
                // delete this line and insert new ones
                lines.erase(lines.begin() + i);
                for (size_t j = 0; j < segments; ++j) {
                    ThickLine new_line(pp[j], pp[j+1]);
                    new_line.a_width = width[2*j];
                    new_line.b_width = width[2*j+1];
                    lines.insert(lines.begin() + i + j, new_line);
                }
                
                --i;
                continue;
            }
            
            const double w = fmax(line.a_width, line.b_width);
            if (path.polyline.points.empty()) {
                path.polyline.append(line.a);
                path.polyline.append(line.b);
                // Convert from spacing to extrusion width based on the extrusion model
                // of a square extrusion ended with semi circles.
                flow.width = unscale(w) + flow.height * (1. - 0.25 * PI);
                #ifdef SLIC3R_DEBUG
                printf("  filling %f gap\n", flow.width);
                #endif
                path.mm3_per_mm  = flow.mm3_per_mm();
                path.width       = flow.width;
                path.height      = flow.height;
            } else {
                thickness_delta = fabs(scale_(flow.width) - w);
                if (thickness_delta <= tolerance/2) {
                    // the width difference between this line and the current flow width is 
                    // within the accepted tolerance
                    path.polyline.append(line.b);
                } else {
                    // we need to initialize a new line
                    paths.emplace_back(std::move(path));
                    path = ExtrusionPath(role);
                    --i;
                }
            }
        }
        if (path.polyline.is_valid())
            paths.emplace_back(std::move(path));        
        // Append paths to collection.
        if (!paths.empty()) {
            if (paths.front().first_point().coincides_with(paths.back().last_point())) {
                coll.append(ExtrusionLoop(paths));
            } else {
                //not a loop : avoid to "sort" it.
                ExtrusionEntityCollection unsortable_coll(paths);
                unsortable_coll.no_sort = true;
                coll.append(unsortable_coll);
            }
        }
    }

    return coll;
}

bool PerimeterGeneratorLoop::is_internal_contour() const
{
    // An internal contour is a contour containing no other contours
    if (! this->is_contour)
                return false;
    for (const PerimeterGeneratorLoop &loop : this->children)
        if (loop.is_contour)
            return false;
        return true;
    }

}
