#include <vtkm/cont/ArrayHandleGroupVec.h>
#include <vtkm/cont/CellSetSingleType.h>

#include <vtkm/exec/CellEdge.h>

#include <vtkm/worklet/AverageByKey.h>
#include <vtkm/worklet/Keys.h>
#include <vtkm/worklet/ScatterCounting.h>

#include <vtkm/io/reader/VTKDataSetReader.h>
#include <vtkm/io/writer/VTKDataSetWriter.h>

#include <vtkm/filter/Contour.h>
#include <vtkm/filter/FilterDataSet.h>

namespace vtkm
{
namespace worklet
{

struct CountEdgesWorklet : vtkm::worklet::WorkletVisitCellsWithPoints
{
  using ControlSignature = void(CellSetIn cellSet, FieldOut numEdges);
  using ExecutionSignature = _2(CellShape, PointCount);

  template<typename CellShapeTag>
  VTKM_EXEC_CONT vtkm::IdComponent operator()(
    CellShapeTag cellShape,
    vtkm::IdComponent numPointsInCell) const
  {
    return vtkm::exec::CellEdgeNumberOfEdges(numPointsInCell, cellShape, *this);
  }
};

struct EdgeIdsWorklet : vtkm::worklet::WorkletVisitCellsWithPoints
{
  using ControlSignature = void(CellSetIn cellSet, FieldOut canonicalIds);
  using ExecutionSignature = void(CellShape cellShape,
                                  PointIndices globalPointIndices,
                                  VisitIndex localEdgeIndex,
                                  _2 canonicalIdOut);

  using ScatterType = vtkm::worklet::ScatterCounting;

  template<typename CellShapeTag, typename PointIndexVecType>
  VTKM_EXEC void operator()(CellShapeTag cellShape,
                            const PointIndexVecType& globalPointIndicesForCell,
                            vtkm::IdComponent localEdgeIndex,
                            vtkm::Id2& canonicalIdOut) const
  {
    vtkm::IdComponent numPointsInCell =
      globalPointIndicesForCell.GetNumberOfComponents();

    canonicalIdOut = vtkm::exec::CellEdgeCanonicalId(
      numPointsInCell, localEdgeIndex, cellShape, globalPointIndicesForCell, *this);
  }
};

struct EdgeIndicesWorklet : vtkm::worklet::WorkletReduceByKey
{
  using ControlSignature = void(KeysIn keys,
                                WholeCellSetIn<> inputCells,
                                ValuesIn originCells,
                                ValuesIn originEdges,
                                ReducedValuesOut connectivityOut);
  using ExecutionSignature = void(_2 inputCells,
                                  _3 originCell,
                                  _4 originEdge,
                                  _5 connectivityOut);
  using InputDomain = _1;

  template<typename CellSetType, typename OriginCellsType, typename OriginEdgesType>
  VTKM_EXEC void operator()(const CellSetType& cellSet,
                            const OriginCellsType& originCells,
                            const OriginEdgesType& originEdges,
                            vtkm::Id2& connectivityOut) const
  {
    // Regardless of how many cells are sharing the edge we are generating, we
    // know that each cell/edge given to us by the reduce-by-key refers to the
    // same edge, so we can just look at the first cell to get the edge.
    vtkm::IdComponent numPointsInCell = cellSet.GetNumberOfIndices(originCells[0]);
    vtkm::IdComponent edgeIndex = originEdges[0];
    auto cellShape = cellSet.GetCellShape(originCells[0]);

    vtkm::IdComponent pointInCellIndex0 = vtkm::exec::CellEdgeLocalIndex(
      numPointsInCell, 0, edgeIndex, cellShape, *this);
    vtkm::IdComponent pointInCellIndex1 = vtkm::exec::CellEdgeLocalIndex(
      numPointsInCell, 1, edgeIndex, cellShape, *this);

    auto globalPointIndicesForCell = cellSet.GetIndices(originCells[0]);
    connectivityOut[0] = globalPointIndicesForCell[pointInCellIndex0];
    connectivityOut[1] = globalPointIndicesForCell[pointInCellIndex1];
  }
};

} // namespace worklet
} // namespace vtkm

namespace vtkm
{
namespace filter
{

class ExtractEdges : public vtkm::filter::FilterDataSet<ExtractEdges>
{
public:
  template<typename Policy>
  VTKM_CONT vtkm::cont::DataSet DoExecute(const vtkm::cont::DataSet& inData,
                                          vtkm::filter::PolicyBase<Policy> policy);

  template<typename T, typename StorageType, typename Policy>
  VTKM_CONT bool DoMapField(vtkm::cont::DataSet& result,
                            const vtkm::cont::ArrayHandle<T, StorageType>& input,
                            const vtkm::filter::FieldMetadata& fieldMeta,
                            const vtkm::filter::PolicyBase<Policy>& policy);

private:
  vtkm::worklet::ScatterCounting::OutputToInputMapType OutputToInputCellMap;
  vtkm::worklet::Keys<vtkm::Id2> CellToEdgeKeys;
};

template<typename Policy>
inline VTKM_CONT vtkm::cont::DataSet ExtractEdges::DoExecute(
  const vtkm::cont::DataSet& inData,
  vtkm::filter::PolicyBase<Policy> policy)
{
  const vtkm::cont::DynamicCellSet& inCellSet =
    vtkm::filter::ApplyPolicyCellSet(inData.GetCellSet(), policy);

  // First, count the edges in each cell.
  vtkm::cont::ArrayHandle<vtkm::IdComponent> edgeCounts;
  this->Invoke(vtkm::worklet::CountEdgesWorklet{}, inCellSet, edgeCounts);

  // Second, using these counts build a scatter that repeats a cell's visit
  // for each edge in the cell.
  vtkm::worklet::ScatterCounting scatter(edgeCounts);
  this->OutputToInputCellMap =
    scatter.GetOutputToInputMap(inCellSet.GetNumberOfCells());
  vtkm::worklet::ScatterCounting::VisitArrayType outputToInputEdgeMap =
    scatter.GetVisitArray(inCellSet.GetNumberOfCells());

  // Third, for each edge, extract a canonical id.
  vtkm::cont::ArrayHandle<vtkm::Id2> canonicalIds;
  this->Invoke(vtkm::worklet::EdgeIdsWorklet{}, scatter, inCellSet, canonicalIds);

  // Fourth, construct a Keys object to combine all like edge ids.
  this->CellToEdgeKeys = vtkm::worklet::Keys<vtkm::Id2>(canonicalIds);

  // Fifth, use a reduce-by-key to extract indices for each unique edge.
  vtkm::cont::ArrayHandle<vtkm::Id> connectivityArray;
  this->Invoke(vtkm::worklet::EdgeIndicesWorklet{},
               this->CellToEdgeKeys,
               inCellSet,
               this->OutputToInputCellMap,
               outputToInputEdgeMap,
               vtkm::cont::make_ArrayHandleGroupVec<2>(connectivityArray));

  // Sixth, use the created connectivity array to build a cell set.
  vtkm::cont::CellSetSingleType<> outCellSet;
  outCellSet.Fill(
    inCellSet.GetNumberOfPoints(), vtkm::CELL_SHAPE_LINE, 2, connectivityArray);

  vtkm::cont::DataSet outData;

  outData.SetCellSet(outCellSet);

  for (vtkm::IdComponent coordSystemIndex = 0;
       coordSystemIndex < inData.GetNumberOfCoordinateSystems();
       ++coordSystemIndex)
  {
    outData.AddCoordinateSystem(inData.GetCoordinateSystem(coordSystemIndex));
  }

  return outData;
}

template<typename T, typename StorageType, typename Policy>
inline VTKM_CONT bool ExtractEdges::DoMapField(
  vtkm::cont::DataSet& result,
  const vtkm::cont::ArrayHandle<T, StorageType>& inputArray,
  const vtkm::filter::FieldMetadata& fieldMeta,
  const vtkm::filter::PolicyBase<Policy>&)
{
  vtkm::cont::Field outputField;

  if (fieldMeta.IsPointField())
  {
    outputField = fieldMeta.AsField(inputArray); // pass through
  }
  else if (fieldMeta.IsCellField())
  {
    auto outputCellArray =
      vtkm::worklet::AverageByKey::Run(this->CellToEdgeKeys,
                                       vtkm::cont::make_ArrayHandlePermutation(
                                         this->OutputToInputCellMap, inputArray));
    outputField = fieldMeta.AsField(outputCellArray);
  }
  else
  {
    return false;
  }

  result.AddField(outputField);

  return true;
}

} // namespace filter
} // namespace vtkm

int main()
{
  const char *input = "kitchen.vtk";
  vtkm::io::reader::VTKDataSetReader reader(input);
  vtkm::cont::DataSet ds_from_file = reader.ReadDataSet();

  vtkm::filter::Contour contour;
  contour.SetActiveField("c1");
  contour.SetIsoValue(0.10);
  vtkm::cont::DataSet ds_from_contour = contour.Execute(ds_from_file);

  vtkm::filter::ExtractEdges extractEdges;
  vtkm::cont::DataSet wireframe = extractEdges.Execute(ds_from_contour);

  vtkm::io::writer::VTKDataSetWriter writer("out_wireframe.vtk");
  writer.WriteDataSet(wireframe);

  return 0;
}
