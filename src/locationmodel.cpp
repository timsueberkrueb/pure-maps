#include "locationmodel.h"
#include "navigator.h"

#include <QLocale>

#include <QDebug>

// use var without m_ prefix
#define SET(var, value) { auto t=(value); if (m_##var != t) { m_##var=t; /*qDebug() << "Emit " #var;*/ emit var##Changed(); } }

LocationModel::LocationModel(Navigator *navigator)
{
  m_navigator = navigator;
  connect(this, &LocationModel::modelAboutToBeReset, this, &LocationModel::dropCache);
  connect(this, &LocationModel::modelReset, this, &LocationModel::dropCache);
}

QHash<int, QByteArray> LocationModel::roleNames() const
{
  return {
      { RoleNames::DestinationRole, QByteArrayLiteral("destination") },
      { RoleNames::OriginRole, QByteArrayLiteral("origin") },
      { RoleNames::FinalRole, QByteArrayLiteral("final") },
      { RoleNames::TextRole, QByteArrayLiteral("text") },
      { RoleNames::XRole, QByteArrayLiteral("x") },
      { RoleNames::YRole, QByteArrayLiteral("y") },
      { RoleNames::DistRole, QByteArrayLiteral("dist") },
      { RoleNames::TimeRole, QByteArrayLiteral("time") },
      { RoleNames::EtaRole, QByteArrayLiteral("eta") }
    };
}

QVariant LocationModel::data(const QModelIndex &index, int role) const
{
  const int row = index.row();
  if (!index.isValid() || row < 0 || row >= m_locations.size())
    return {};

  const Location &loc = m_locations[row];
  switch (role) {
    case RoleNames::DestinationRole:
      return loc.destination ||
          (row == m_locations.length()-1 &&
           m_locations.length() >= 1 && m_hasDestination);
    case RoleNames::OriginRole:
      return row == 0 && m_locations.length() >= 1 && m_hasOrigin;
    case RoleNames::FinalRole:
      return
          row == m_locations.length()-1 &&
          m_locations.length() >= 1 && m_hasDestination;
    case RoleNames::TextRole:
      return loc.name;
    case RoleNames::XRole:
      return loc.longitude;
    case RoleNames::YRole:
      return loc.latitude;
    case RoleNames::DistRole:
      return loc.dist;
    case RoleNames::TimeRole:
      return loc.time;
    case RoleNames::EtaRole:
      return loc.eta;
    }

  return {};
}

int LocationModel::rowCount(const QModelIndex &parent) const
{
  return parent.isValid() ? 0 : m_locations.size();
}

void LocationModel::dropCache()
{
  m_locations_cached_ready = false;
}

QVariantList LocationModel::list()
{
  if (m_locations_cached_ready && m_locations_cached.size()==m_locations.size())
    return m_locations_cached;

  QVariantList locations;

  for (auto l: m_locations)
    {
      QVariantMap loc;
      loc["text"] = l.name;
      loc["x"] = l.longitude;
      loc["y"] = l.latitude;
      loc["destination"] = (l.destination ? 1 : 0);
      locations.append(loc);
    }

  // set origin and final destinations
  if (m_locations.length() >= 1 && m_hasOrigin)
    {
      QVariantMap lo = locations.front().toMap();
      lo["origin"] = 1;
      locations.front() = lo;
    }

  if (m_locations.length() >= 1 && m_hasDestination)
    {
      QVariantMap lf = locations.last().toMap();
      lf["final"] = 1;
      lf["destination"] = 1;
      locations.last() = lf;
    }

  m_locations_cached = locations;
  m_locations_cached_ready = true;
  return locations;
}

void LocationModel::checkArrivalByPosition(const S2Point &point, double accuracy)
{
  // clear destination if we are close to it
  for (int i=m_locations.length()-1; i>=0; --i)
    if ( m_locations[i].destination &&
         !m_locations[i].origin && !m_locations[i].final &&
         m_locations[i].closeToRoutePoint(point, accuracy) )
      {
        qDebug() << "Arrived to location" << i << m_locations[i].name;
        emit locationArrived(m_locations[i].name, m_locations[i].destination);
        beginResetModel();
        m_locations.removeAt(i);
        endResetModel();
      }
}

void LocationModel::checkArrivalByRouteDistance(double length_on_route, double accuracy)
{
  for (int i=m_locations.length()-1; i>=0; --i)
    if (!m_locations[i].origin && !m_locations[i].final &&
        ( (!m_locations[i].destination &&
           m_locations[i].length_on_route < length_on_route) ||
          (m_locations[i].destination &&
           abs(m_locations[i].length_on_route - length_on_route) <
               m_locations[i].distance_to_route + accuracy) ) )
      {
        // clear waypoint iff the destinations before
        // it have been cleared already
        for (int j=0; j < i; ++j)
          if (!m_locations[j].origin && m_locations[j].destination)
            continue; // skip removal

        qDebug() << "Arrived to location along route" << i << m_locations[i].name;
        emit locationArrived(m_locations[i].name, m_locations[i].destination);
        beginResetModel();
        m_locations.removeAt(i);
        endResetModel();
      }
}

bool LocationModel::hasMissedDest(double length_on_route, double accuracy)
{
  // returns true if some destination was missed
  for (int i=0; i < m_locations.length(); ++i)
    if (!m_locations[i].origin && !m_locations[i].final &&
         m_locations[i].destination &&
         m_locations[i].length_on_route < length_on_route - accuracy )
        return true;
  return false;
}

#define SETLOC(var, value, role) { \
  auto t=(value); \
  if (var != t) { var=t; roles.push_back(role); } }

void LocationModel::updateRoutePosition(double last_distance_along_route_m,
                                        double last_duration_along_route)
{
  for (int i=1; i < m_locations.length()-1; ++i)
    {
      Location &loc = m_locations[i];
      QVector<int> roles;

      SETLOC(loc.dist,
             m_navigator->distanceToStr(loc.length_on_route_m -
                                        last_distance_along_route_m),
             RoleNames::DistRole);
      SETLOC(loc.time,
             m_navigator->timeToStr(loc.duration_on_route -
                                    last_duration_along_route),
             RoleNames::TimeRole);

      if (loc.duration_on_route > last_duration_along_route)
        {
          QTime time = QTime::currentTime().addSecs(loc.duration_on_route -
                                                    last_duration_along_route);
          SETLOC(loc.eta,
                 QLocale::system().toString(time, QLocale::NarrowFormat),
                 RoleNames::EtaRole);
        }
      else
        SETLOC(loc.eta, QLatin1String(), RoleNames::EtaRole);

      if (roles.length() > 0)
        {
          QModelIndex index = createIndex(i, 0);
          emit dataChanged(index, index, roles);
        }

      if (i==1)
        {
          SET(hasNextLocation, true);
          SET(nextLocationDestination, loc.destination);
          SET(nextLocationDist, loc.dist);
          SET(nextLocationEta, loc.eta);
          SET(nextLocationTime, loc.time);
        }
    }
}


void LocationModel::append(const Location &location)
{
  beginResetModel();
  m_locations.push_back(location);
  endResetModel();
}

void LocationModel::clear()
{
  beginResetModel();
  SET(hasDestination, false);
  SET(hasOrigin, false);
  m_locations.clear();
  endResetModel();
}

void LocationModel::set(const QVariantList &locations)
{
  beginResetModel();

  for (const QVariant &val: locations)
    {
      QVariantMap location = val.toMap();
      Location loc(location);
      m_locations.append(loc);
      // check for origin
      if (m_locations.length() == 1)
        SET(hasOrigin, location.value("origin", false).toBool());
    }

  if (m_locations.length() > 0 && !m_locations.back().origin)
    SET(hasDestination, m_locations.back().destination);

  endResetModel();
}

void LocationModel::set(const QList<Location> &locations)
{
  beginResetModel();
  SET(hasOrigin, true);
  SET(hasDestination, true);
  m_locations = locations;
  endResetModel();
}

bool LocationModel::remove(int index)
{
  if (index < 0 || index >= m_locations.length())
    return false;

  beginResetModel();
  m_locations.removeAt(index);
  endResetModel();

  if (index == 0) SET(hasOrigin, false);
  if (index == m_locations.length())
    SET(hasDestination,
        m_locations.length() > 1 ||
        (!m_hasOrigin && m_locations.length() > 0) ?
          m_locations.back().destination : false);

  return true;
}