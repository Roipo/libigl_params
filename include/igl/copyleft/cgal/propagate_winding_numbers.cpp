// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#include "propagate_winding_numbers.h"
#include "../../extract_manifold_patches.h"
#include "../../extract_non_manifold_edge_curves.h"
#include "../../facet_components.h"
#include "../../unique_edge_map.h"
#include "../../writeOBJ.h"
#include "../../writePLY.h"
#include "order_facets_around_edge.h"
#include "outer_facet.h"
#include "closest_facet.h"
#include "assign_scalar.h"
#include "extract_cells.h"

#include <stdexcept>
#include <limits>
#include <vector>
#include <tuple>
#include <queue>

namespace propagate_winding_numbers_helper {
  template<
    typename DerivedF,
    typename DeriveduE,
    typename uE2EType >
  bool is_orientable(
      const Eigen::PlainObjectBase<DerivedF>& F,
      const Eigen::PlainObjectBase<DeriveduE>& uE,
      const std::vector<std::vector<uE2EType> >& uE2E) {
    const size_t num_faces = F.rows();
    const size_t num_edges = uE.rows();
    auto edge_index_to_face_index = [&](size_t ei) {
      return ei % num_faces;
    };
    auto is_consistent = [&](size_t fid, size_t s, size_t d) {
      if ((size_t)F(fid, 0) == s && (size_t)F(fid, 1) == d) return true;
      if ((size_t)F(fid, 1) == s && (size_t)F(fid, 2) == d) return true;
      if ((size_t)F(fid, 2) == s && (size_t)F(fid, 0) == d) return true;

      if ((size_t)F(fid, 0) == d && (size_t)F(fid, 1) == s) return false;
      if ((size_t)F(fid, 1) == d && (size_t)F(fid, 2) == s) return false;
      if ((size_t)F(fid, 2) == d && (size_t)F(fid, 0) == s) return false;
      throw "Invalid face!!";
    };
    for (size_t i=0; i<num_edges; i++) {
      const size_t s = uE(i,0);
      const size_t d = uE(i,1);
      int count=0;
      for (const auto& ei : uE2E[i]) {
        const size_t fid = edge_index_to_face_index(ei);
        if (is_consistent(fid, s, d)) count++;
        else count--;
      }
      if (count != 0) {
        return false;
      }
    }
    return true;
  }
}

template<
  typename DerivedV,
  typename DerivedF,
  typename DerivedL,
  typename DerivedW>
IGL_INLINE void igl::copyleft::cgal::propagate_winding_numbers(
    const Eigen::PlainObjectBase<DerivedV>& V,
    const Eigen::PlainObjectBase<DerivedF>& F,
    const Eigen::PlainObjectBase<DerivedL>& labels,
    Eigen::PlainObjectBase<DerivedW>& W) {
  const size_t num_faces = F.rows();
  //typedef typename DerivedF::Scalar Index;

  Eigen::MatrixXi E, uE;
  Eigen::VectorXi EMAP;
  std::vector<std::vector<size_t> > uE2E;
  igl::unique_edge_map(F, E, uE, EMAP, uE2E);
  if (!propagate_winding_numbers_helper::is_orientable(F, uE, uE2E)) {
      std::cerr << "Input mesh is not orientable!" << std::endl;
  }

  Eigen::VectorXi P;
  const size_t num_patches = igl::extract_manifold_patches(F, EMAP, uE2E, P);

  DerivedW per_patch_cells;
  const size_t num_cells =
    igl::copyleft::cgal::extract_cells(
        V, F, P, E, uE, uE2E, EMAP, per_patch_cells);

  typedef std::tuple<size_t, bool, size_t> CellConnection;
  std::vector<std::set<CellConnection> > cell_adjacency(num_cells);
  for (size_t i=0; i<num_patches; i++) {
    const int positive_cell = per_patch_cells(i,0);
    const int negative_cell = per_patch_cells(i,1);
    cell_adjacency[positive_cell].emplace(negative_cell, false, i);
    cell_adjacency[negative_cell].emplace(positive_cell, true, i);
  }

  auto save_cell = [&](const std::string& filename, size_t cell_id) {
    std::vector<size_t> faces;
    for (size_t i=0; i<num_patches; i++) {
      if ((per_patch_cells.row(i).array() == cell_id).any()) {
        for (size_t j=0; j<num_faces; j++) {
          if ((size_t)P[j] == i) {
            faces.push_back(j);
          }
        }
      }
    }
    Eigen::MatrixXi cell_faces(faces.size(), 3);
    for (size_t i=0; i<faces.size(); i++) {
      cell_faces.row(i) = F.row(faces[i]);
    }
    Eigen::MatrixXd vertices(V.rows(), 3);
    for (size_t i=0; i<(size_t)V.rows(); i++) {
      assign_scalar(V(i,0), vertices(i,0));
      assign_scalar(V(i,1), vertices(i,1));
      assign_scalar(V(i,2), vertices(i,2));
    }
    writePLY(filename, vertices, cell_faces);
  };

#ifndef NDEBUG
  {
    // Check for odd cycle.
    Eigen::VectorXi cell_labels(num_cells);
    cell_labels.setZero();
    Eigen::VectorXi parents(num_cells);
    parents.setConstant(-1);
    auto trace_parents = [&](size_t idx) {
      std::list<size_t> path;
      path.push_back(idx);
      while ((size_t)parents[path.back()] != path.back()) {
        path.push_back(parents[path.back()]);
      }
      return path;
    };
    for (size_t i=0; i<num_cells; i++) {
      if (cell_labels[i] == 0) {
        cell_labels[i] = 1;
        std::queue<size_t> Q;
        Q.push(i);
        parents[i] = i;
        while (!Q.empty()) {
          size_t curr_idx = Q.front();
          Q.pop();
          int curr_label = cell_labels[curr_idx];
          for (const auto& neighbor : cell_adjacency[curr_idx]) {
            if (cell_labels[std::get<0>(neighbor)] == 0) {
              cell_labels[std::get<0>(neighbor)] = curr_label * -1;
              Q.push(std::get<0>(neighbor));
              parents[std::get<0>(neighbor)] = curr_idx;
            } else {
              if (cell_labels[std::get<0>(neighbor)] !=
                  curr_label * -1) {
                std::cerr << "Odd cell cycle detected!" << std::endl;
                auto path = trace_parents(curr_idx);
                path.reverse();
                auto path2 = trace_parents(std::get<0>(neighbor));
                path.insert(path.end(),
                    path2.begin(), path2.end());
                for (auto cell_id : path) {
                  std::cout << cell_id << " ";
                  std::stringstream filename;
                  filename << "cell_" << cell_id << ".ply";
                  save_cell(filename.str(), cell_id);
                }
                std::cout << std::endl;
              }
              assert(cell_labels[std::get<0>(neighbor)] == curr_label * -1);
            }
          }
        }
      }
    }
  }
#endif

  size_t outer_facet;
  bool flipped;
  Eigen::VectorXi I;
  I.setLinSpaced(num_faces, 0, num_faces-1);
  igl::copyleft::cgal::outer_facet(V, F, I, outer_facet, flipped);

  const size_t outer_patch = P[outer_facet];
  const size_t infinity_cell = per_patch_cells(outer_patch, flipped?1:0);

  Eigen::VectorXi patch_labels(num_patches);
  const int INVALID = std::numeric_limits<int>::max();
  patch_labels.setConstant(INVALID);
  for (size_t i=0; i<num_faces; i++) {
    if (patch_labels[P[i]] == INVALID) {
      patch_labels[P[i]] = labels[i];
    } else {
      assert(patch_labels[P[i]] == labels[i]);
    }
  }
  assert((patch_labels.array() != INVALID).all());
  const size_t num_labels = patch_labels.maxCoeff()+1;

  Eigen::MatrixXi per_cell_W(num_cells, num_labels);
  per_cell_W.setConstant(INVALID);
  per_cell_W.row(infinity_cell).setZero();
  std::queue<size_t> Q;
  Q.push(infinity_cell);
  while (!Q.empty()) {
    size_t curr_cell = Q.front();
    Q.pop();
    for (const auto& neighbor : cell_adjacency[curr_cell]) {
      size_t neighbor_cell, patch_idx;
      bool direction;
      std::tie(neighbor_cell, direction, patch_idx) = neighbor;
      if ((per_cell_W.row(neighbor_cell).array() == INVALID).any()) {
        per_cell_W.row(neighbor_cell) = per_cell_W.row(curr_cell);
        for (size_t i=0; i<num_labels; i++) {
          int inc = (patch_labels[patch_idx] == (int)i) ?
            (direction ? -1:1) :0;
          per_cell_W(neighbor_cell, i) =
            per_cell_W(curr_cell, i) + inc;
        }
        Q.push(neighbor_cell);
      } else {
#ifndef NDEBUG
        for (size_t i=0; i<num_labels; i++) {
          if ((int)i == patch_labels[patch_idx]) {
            int inc = direction ? -1:1;
            assert(per_cell_W(neighbor_cell, i) ==
                per_cell_W(curr_cell, i) + inc);
          } else {
            assert(per_cell_W(neighbor_cell, i) ==
                per_cell_W(curr_cell, i));
          }
        }
#endif
      }
    }
  }

  W.resize(num_faces, num_labels*2);
  for (size_t i=0; i<num_faces; i++) {
    const size_t patch = P[i];
    const size_t positive_cell = per_patch_cells(patch, 0);
    const size_t negative_cell = per_patch_cells(patch, 1);
    for (size_t j=0; j<num_labels; j++) {
      W(i,j*2  ) = per_cell_W(positive_cell, j);
      W(i,j*2+1) = per_cell_W(negative_cell, j);
    }
  }
}


#ifdef IGL_STATIC_LIBRARY
template void igl::copyleft::cgal::propagate_winding_numbers<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
