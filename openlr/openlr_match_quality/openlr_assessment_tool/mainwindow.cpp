#include "openlr/openlr_match_quality/openlr_assessment_tool/mainwindow.hpp"

#include "openlr/openlr_match_quality/openlr_assessment_tool/traffic_panel.hpp"
#include "openlr/openlr_match_quality/openlr_assessment_tool/trafficmodeinitdlg.h"
#include "openlr/openlr_match_quality/openlr_assessment_tool/map_widget.hpp"

#include "map/framework.hpp"

#include "drape_frontend/drape_api.hpp"

#include "routing/features_road_graph.hpp"
#include "routing/road_graph.hpp"

#include "routing_common/car_model.hpp"

#include "geometry/mercator.hpp"
#include "geometry/point2d.hpp"

#include <QDockWidget>
#include <QFileDialog>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStandardPaths>

#include <cerrno>
#include <cstring>
#include <vector>

namespace
{
class TrafficDrawerDelegate : public TrafficDrawerDelegateBase
{
public:
  TrafficDrawerDelegate(Framework & framework)
    : m_framework(framework)
    , m_drapeApi(m_framework.GetDrapeApi())
    , m_bm(framework.GetBookmarkManager())
  {
  }

  void SetViewportCenter(m2::PointD const & center) override
  {
    m_framework.SetViewportCenter(center);
  }

  void DrawDecodedSegments(std::vector<m2::PointD> const & points) override
  {
    CHECK(!points.empty(), ("Points must not be empty."));

    LOG(LINFO, ("Decoded segment", points));
    m_drapeApi.AddLine(NextLineId(),
                       df::DrapeApiLineData(points, dp::Color(0, 0, 255, 255))
                       .Width(3.0f).ShowPoints(true /* markPoints */));
  }

  void DrawEncodedSegment(std::vector<m2::PointD> const & points) override
  {
    LOG(LINFO, ("Encoded segment", points));
    m_drapeApi.AddLine(NextLineId(),
                       df::DrapeApiLineData(points, dp::Color(255, 0, 0, 255))
                       .Width(3.0f).ShowPoints(true /* markPoints */));
  }

  void ClearAllPaths() override
  {
    m_drapeApi.Clear();
  }

  void VisualizeGoldenPath(std::vector<m2::PointD> const & points) override
  {
    ClearAllPaths();

    m_drapeApi.AddLine(NextLineId(),
                       df::DrapeApiLineData(points, dp::Color(255, 127, 36, 255))
                       .Width(4.0f).ShowPoints(true /* markPoints */));
  }

  void VisualizePoints(std::vector<m2::PointD> const & points) override
  {
    UserMarkControllerGuard g(m_bm, UserMarkType::DEBUG_MARK);
    g.m_controller.SetIsVisible(true);
    g.m_controller.SetIsDrawable(true);
    for (auto const & p : points)
      g.m_controller.CreateUserMark(p);
  }

  void CleanAllVisualizedPoints() override
  {
    UserMarkControllerGuard g(m_bm, UserMarkType::DEBUG_MARK);
    g.m_controller.Clear();
  }

private:
  string NextLineId() { return strings::to_string(m_lineId++); }

  uint32_t m_lineId = 0;

  Framework & m_framework;
  df::DrapeApi & m_drapeApi;
  BookmarkManager & m_bm;
};

bool PointsMatch(m2::PointD const & a, m2::PointD const & b)
{
  auto constexpr kToleraneDistanceM = 1.0;
  return MercatorBounds::DistanceOnEarth(a, b) < kToleraneDistanceM;
}

class PointsControllerDelegate : public PointsControllerDelegateBase
{
public:
  PointsControllerDelegate(Framework & framework)
    : m_framework(framework)
    , m_index(framework.GetIndex())
    , m_roadGraph(m_index, routing::IRoadGraph::Mode::ObeyOnewayTag,
                  make_unique<routing::CarModelFactory>())
  {
  }

  std::vector<m2::PointD> GetAllJunctionPointsInViewPort() const
  {
    std::vector<m2::PointD> points;
    auto const & rect = m_framework.GetCurrentViewport();
    auto const pushPoint = [&points, &rect](m2::PointD const & point) {
      if (!rect.IsPointInside(point))
        return;
      for (auto const & p : points)
      {
        if (PointsMatch(point, p))
          return;
      }
      points.push_back(point);
    };

    auto const pushFeaturePoints = [&pushPoint](FeatureType & ft) {
      if (ft.GetFeatureType() != feature::GEOM_LINE)
        return;
      auto const roadClass = ftypes::GetHighwayClass(ft);
      if (roadClass == ftypes::HighwayClass::Error ||
          roadClass == ftypes::HighwayClass::Pedestrian)
      {
        return;
      }
      ft.ForEachPoint(pushPoint, scales::GetUpperScale());
    };

    m_index.ForEachInRect(pushFeaturePoints, rect, scales::GetUpperScale());
    return points;
  }

  std::vector<FeaturePoint> GetFeaturesPointsByPoint(m2::PointD & p) const
  {
    std::vector<FeaturePoint> points;
    m2::PointD pointOnFt;
    indexer::ForEachFeatureAtPoint(m_index, [&points, &p, &pointOnFt](FeatureType & ft) {
        if (ft.GetFeatureType() != feature::GEOM_LINE)
          return;
        size_t pointIndex = 0;
        auto minDistance = std::numeric_limits<double>::max();

        ft.ForEachPoint(
            [&points, &p, &ft, &pointIndex, &minDistance, &pointOnFt](m2::PointD const & fp)
            {
              auto const distance = MercatorBounds::DistanceOnEarth(fp, p);
              if (PointsMatch(fp, p) && distance < minDistance)
              {
                points.emplace_back(ft.GetID(), pointIndex);
                pointOnFt = fp;
                minDistance = distance;
              }
              ++pointIndex;
            },
            FeatureType::BEST_GEOMETRY);
      },
      p);
    p = pointOnFt;
    return points;
  }

  std::vector<m2::PointD> GetReachablePoints(m2::PointD const & p) const
  {
    routing::FeaturesRoadGraph::TEdgeVector edges;
    m_roadGraph.GetOutgoingEdges(
        routing::Junction(p, 0 /* altitude */),
        edges);

    std::vector<m2::PointD> points;
    for (auto const & e : edges)
      points.push_back(e.GetEndJunction().GetPoint());
    return points;
  }

  ClickType CheckClick(m2::PointD const & clickPoint,
                       m2::PointD const & lastClickedPoint,
                       std::vector<m2::PointD> const & reachablePoints) const
  {
    // == Comparison is safe here since |clickPoint| is adjusted by GetFeaturesPointsByPoint
    // so to be equal the closest feature's one.
    if (clickPoint == lastClickedPoint)
      return ClickType::Remove;
    for (auto const & p : reachablePoints)
    {
      if (PointsMatch(clickPoint, p));
        return ClickType::Add;
    }
    return ClickType::Miss;
  }

private:
  Framework & m_framework;
  Index const & m_index;
  routing::FeaturesRoadGraph m_roadGraph;
};
}  // namespace


MainWindow::MainWindow(Framework & framework)
  : m_framework(framework)
{
  m_mapWidget = new MapWidget(
      m_framework, false /* apiOpenGLES3 */, this /* parent */
  );
  setCentralWidget(m_mapWidget);

  // setWindowTitle(tr("MAPS.ME"));
  // setWindowIcon(QIcon(":/ui/logo.png"));

  QMenu * fileMenu = new QMenu("File", this);
  menuBar()->addMenu(fileMenu);

  fileMenu->addAction("Open sample", this, &MainWindow::OnOpenTrafficSample);

  m_closeTrafficSampleAction = fileMenu->addAction(
      "Close sample", this, &MainWindow::OnCloseTrafficSample
  );
  m_closeTrafficSampleAction->setEnabled(false /* enabled */);

  m_saveTrafficSampleAction = fileMenu->addAction(
      "Save sample", this, &MainWindow::OnSaveTrafficSample
  );

  fileMenu->addAction("Start editing", [this] {
      m_trafficMode->StartBuildingPath();
      m_mapWidget->SetTrafficMarkupMode();
      //m_mapWidget->ShowPointsInViewPort();
  });
  fileMenu->addAction("Commit path", [this] {
      m_trafficMode->CommitPath();
      m_mapWidget->SetNormalMode();
      //TODO m_mapWidget->ClearShownPoints();
  });
  fileMenu->addAction("Cancel path", [this] {
      m_trafficMode->RollBackPath();
      m_mapWidget->SetNormalMode();
      // TODO m_mapWidget->ClearShownPoints();
  });

  m_saveTrafficSampleAction->setEnabled(false /* enabled */);
}

void MainWindow::CreateTrafficPanel(string const & dataFilePath)
{
  m_trafficMode = new TrafficMode(dataFilePath,
                                  m_framework.GetIndex(),
                                  make_unique<TrafficDrawerDelegate>(m_framework),
                                  make_unique<PointsControllerDelegate>(m_framework));

  connect(m_mapWidget, &MapWidget::TrafficMarkupClick,
          m_trafficMode, &TrafficMode::OnClick);

  m_docWidget = new QDockWidget(tr("Routes"), this);
  addDockWidget(Qt::DockWidgetArea::RightDockWidgetArea, m_docWidget);

  m_docWidget->setWidget(new TrafficPanel(m_trafficMode, m_docWidget));

  m_docWidget->adjustSize();
  m_docWidget->show();
}

void MainWindow::DestroyTrafficPanel()
{
  removeDockWidget(m_docWidget);
  delete m_docWidget;
  m_docWidget = nullptr;

  delete m_trafficMode;
  m_trafficMode = nullptr;

  m_mapWidget->SetNormalMode();
}

void MainWindow::OnOpenTrafficSample()
{
  TrafficModeInitDlg dlg;
  dlg.exec();
  if (dlg.result() != QDialog::DialogCode::Accepted)
    return;

  try
  {
    CreateTrafficPanel(dlg.GetDataFilePath());
  }
  catch (TrafficModeError const & e)
  {
    QMessageBox::critical(this, "Data loading error", QString("Can't load data file."));
    LOG(LERROR, (e.Msg()));
    return;
  }

  m_closeTrafficSampleAction->setEnabled(true /* enabled */);
  m_saveTrafficSampleAction->setEnabled(true /* enabled */);
}

void MainWindow::OnCloseTrafficSample()
{
  // TODO(mgsergio):
  // If not saved, ask a user if he/she wants to save.
  // OnSaveTrafficSample()

  m_saveTrafficSampleAction->setEnabled(false /* enabled */);
  m_closeTrafficSampleAction->setEnabled(false /* enabled */);
  DestroyTrafficPanel();
}

void MainWindow::OnSaveTrafficSample()
{
  // TODO(mgsergio): Add default filename.
  auto const & fileName = QFileDialog::getSaveFileName(this, "Save sample");
  if (fileName.isEmpty())
    return;

  if (!m_trafficMode->SaveSampleAs(fileName.toStdString()))
  {
    QMessageBox::critical(
        this, "Saving error",
        QString("Can't save file: ") + strerror(errno));
  }
}