/**************************************************************************
 *                                                                        *
 * Copyright (C) 2015 Felix Rohrbach <kde@fxrh.de>                        *
 *                                                                        *
 * This program is free software; you can redistribute it and/or          *
 * modify it under the terms of the GNU General Public License            *
 * as published by the Free Software Foundation; either version 3         *
 * of the License, or (at your option) any later version.                 *
 *                                                                        *
 * This program is distributed in the hope that it will be useful,        *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 * GNU General Public License for more details.                           *
 *                                                                        *
 * You should have received a copy of the GNU General Public License      *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  *
 *                                                                        *
 **************************************************************************/

#include "messageeventmodel.h"

#include <QtCore/QSettings>
#include <QtCore/QDebug>
#include <QtQml> // for qmlRegisterType()

#include "../quaternionroom.h"
#include <connection.h>
#include <user.h>
#include <settings.h>
#include <events/roommemberevent.h>
#include <events/simplestateevents.h>
#include <events/redactionevent.h>
#include <events/roomavatarevent.h>

enum EventRoles {
    EventTypeRole = Qt::UserRole + 1,
    EventIdRole,
    TimeRole,
    SectionRole,
    AboveSectionRole,
    AuthorRole,
    AboveAuthorRole,
    ContentRole,
    ContentTypeRole,
    HighlightRole,
    ReadMarkerRole,
    SpecialMarksRole,
    LongOperationRole,
    // For debugging
    EventResolvedTypeRole,
};

QHash<int, QByteArray> MessageEventModel::roleNames() const
{
    QHash<int, QByteArray> roles = QAbstractItemModel::roleNames();
    roles[EventTypeRole] = "eventType";
    roles[EventIdRole] = "eventId";
    roles[TimeRole] = "time";
    roles[SectionRole] = "section";
    roles[AboveSectionRole] = "aboveSection";
    roles[AuthorRole] = "author";
    roles[AboveAuthorRole] = "aboveAuthor";
    roles[ContentRole] = "content";
    roles[ContentTypeRole] = "contentType";
    roles[HighlightRole] = "highlight";
    roles[ReadMarkerRole] = "readMarker";
    roles[SpecialMarksRole] = "marks";
    roles[LongOperationRole] = "progressInfo";
    roles[EventResolvedTypeRole] = "eventResolvedType";
    return roles;
}

MessageEventModel::MessageEventModel(QObject* parent)
    : QAbstractListModel(parent)
    , m_currentRoom(nullptr)
{
    qmlRegisterType<QMatrixClient::FileTransferInfo>();
    qRegisterMetaType<QMatrixClient::FileTransferInfo>();
}

void MessageEventModel::changeRoom(QuaternionRoom* room)
{
    if (room == m_currentRoom)
        return;

    beginResetModel();
    if( m_currentRoom )
    {
        m_currentRoom->disconnect( this );
        qDebug() << "Disconnected from" << m_currentRoom->id();
    }

    m_currentRoom = room;
    if( room )
    {
        lastReadEventId = room->readMarkerEventId();
        using namespace QMatrixClient;
        connect(m_currentRoom, &Room::aboutToAddNewMessages, this,
                [=](RoomEventsRange events)
                {
                    const auto pos = m_currentRoom->pendingEvents().size();
                    beginInsertRows(QModelIndex(),
                        int(pos), int(pos + events.size() - 1));
                });
        connect(m_currentRoom, &Room::aboutToAddHistoricalMessages, this,
                [=](RoomEventsRange events)
                {
                    if (rowCount() > 0)
                        nextNewerRow = rowCount() - 1; // See #312
                    beginInsertRows(QModelIndex(), rowCount(),
                                    rowCount() + int(events.size()) - 1);
                });
        connect(m_currentRoom, &Room::addedMessages, this,
                [=] {
                    if (nextNewerRow > -1)
                    {
                        const auto idx = index(nextNewerRow);
                        emit dataChanged(idx, idx);
                        nextNewerRow = -1;
                    }
                    endInsertRows();
                });
        connect(m_currentRoom, &Room::pendingEventAboutToAdd, this,
                [this] { beginInsertRows({}, 0, 0); });
        connect(m_currentRoom, &Room::pendingEventAdded,
                this, &MessageEventModel::endInsertRows);
        connect(m_currentRoom, &Room::pendingEventAboutToMerge, this,
                [this] (RoomEvent*, int i)
                {
                    const auto timelineBaseIdx =
                            int(m_currentRoom->pendingEvents().size());
                    if (i + 1 == timelineBaseIdx)
                        return; // No need to move anything
                    mergingEcho = true;
                    Q_ASSERT(beginMoveRows({}, i, i, {}, timelineBaseIdx));
                });
        connect(m_currentRoom, &Room::pendingEventMerged, this,
                [this] {
                    if (mergingEcho)
                    {
                        endMoveRows();
                        mergingEcho = false;
                    }
                    refreshEventRoles(int(m_currentRoom->pendingEvents().size()),
                                      { SpecialMarksRole });
                });
        connect(m_currentRoom, &Room::pendingEventChanged, this,
                [this] (int i) { refreshEventRoles(i, { SpecialMarksRole }); });
        connect(m_currentRoom, &Room::readMarkerMoved, this, [this] {
            refreshEventRoles(
                std::exchange(lastReadEventId,
                              m_currentRoom->readMarkerEventId()),
                {ReadMarkerRole});
            refreshEventRoles(lastReadEventId, {ReadMarkerRole});
        });
        connect(m_currentRoom, &Room::replacedEvent, this,
                [this] (const RoomEvent* newEvent) {
                    refreshEvent(newEvent->id());
                });
        connect(m_currentRoom, &Room::fileTransferProgress,
                this, &MessageEventModel::refreshEvent);
        connect(m_currentRoom, &Room::fileTransferCompleted,
                this, &MessageEventModel::refreshEvent);
        connect(m_currentRoom, &Room::fileTransferFailed,
                this, &MessageEventModel::refreshEvent);
        connect(m_currentRoom, &Room::fileTransferCancelled,
                this, &MessageEventModel::refreshEvent);
        qDebug() << "Connected to room" << room->id()
                 << "as" << room->localUser()->id();
    } else
        lastReadEventId.clear();
    endResetModel();
}

void MessageEventModel::refreshEvent(const QString& eventId)
{
    refreshEventRoles(eventId, {});
}

void MessageEventModel::refreshEventRoles(const int row,
                                          const QVector<int>& roles)
{
    const auto idx = index(row);
    emit dataChanged(idx, idx, roles);
}

void MessageEventModel::refreshEventRoles(const QString& eventId,
                                          const QVector<int>& roles)
{
    const auto it = m_currentRoom->findInTimeline(eventId);
    if (it != m_currentRoom->timelineEdge())
        refreshEventRoles(it - m_currentRoom->messageEvents().rbegin(), roles);
}

inline bool hasValidTimestamp(const QMatrixClient::TimelineItem& ti)
{
    return ti->timestamp().isValid();
}

QDateTime MessageEventModel::makeMessageTimestamp(
            const QuaternionRoom::rev_iter_t& baseIt) const
{
    const auto& timeline = m_currentRoom->messageEvents();
    auto ts = baseIt->event()->timestamp();
    if (ts.isValid())
        return ts;

    // The event is most likely redacted or just invalid.
    // Look for the nearest date around and slap zero time to it.
    using QMatrixClient::TimelineItem;
    auto rit = std::find_if(baseIt, timeline.rend(),
                      hasValidTimestamp);
    if (rit != timeline.rend())
        return { rit->event()->timestamp().date(), {0,0}, Qt::LocalTime };
    auto it = std::find_if(baseIt.base(), timeline.end(), hasValidTimestamp);
    if (it != timeline.end())
        return { it->event()->timestamp().date(), {0,0}, Qt::LocalTime };

    // What kind of room is that?..
    qCritical() << "No valid timestamps in the room timeline!";
    return {};
}

QString MessageEventModel::makeDateString(
            const QuaternionRoom::rev_iter_t& baseIt) const
{
    auto date = makeMessageTimestamp(baseIt).toLocalTime().date();
    if (QMatrixClient::SettingsGroup("UI")
            .value("banner_human_friendly_date", true).toBool())
    {
        if (date == QDate::currentDate())
            return tr("Today");
        if (date == QDate::currentDate().addDays(-1))
            return tr("Yesterday");
        if (date == QDate::currentDate().addDays(-2))
            return tr("The day before yesterday");
        if (date > QDate::currentDate().addDays(-7))
            return date.toString("dddd");
    }
    return date.toString(Qt::DefaultLocaleShortDate);
}

int MessageEventModel::rowCount(const QModelIndex& parent) const
{
    if( !m_currentRoom || parent.isValid() )
        return 0;
    return m_currentRoom->timelineSize();
}

QVariant MessageEventModel::data(const QModelIndex& index, int role) const
{
    const auto row = index.row();

    if( !m_currentRoom || row < 0 ||
            row >= int(m_currentRoom->pendingEvents().size()) +
                       m_currentRoom->timelineSize())
        return {};

    const auto timelineBaseIdx = int(m_currentRoom->pendingEvents().size());
    const auto timelineIt = m_currentRoom->messageEvents().crbegin() +
                                std::max(-1, row - timelineBaseIdx);
    const auto& evt = row < timelineBaseIdx
                        ? *m_currentRoom->pendingEvents()[size_t(row)]
                        : *timelineIt->event();

    using namespace QMatrixClient;
    if( role == Qt::DisplayRole )
    {
        if (evt.isRedacted())
        {
            auto reason = evt.redactedBecause()->reason();
            if (reason.isEmpty())
                return tr("Redacted");

            return tr("Redacted: %1")
                .arg(evt.redactedBecause()->reason());
        }

        return visit(evt
            , [this] (const RoomMessageEvent& e) {
                using namespace MessageEventContent;

                if (e.hasTextContent() && e.mimeType().name() != "text/plain")
                    return static_cast<const TextContent*>(e.content())->body;
                if (e.hasFileContent())
                {
                    auto fileCaption = e.content()->fileInfo()->originalName;
                    if (fileCaption.isEmpty())
                        fileCaption = m_currentRoom->prettyPrint(e.plainBody());
                    if (fileCaption.isEmpty())
                        return tr("a file");
                }
                return m_currentRoom->prettyPrint(e.plainBody());
            }
            , [this] (const RoomMemberEvent& e) {
                // FIXME: Rewind to the name that was at the time of this event
                QString subjectName = m_currentRoom->roomMembername(e.userId());
                // The below code assumes senderName output in AuthorRole
                switch( e.membership() )
                {
                    case MembershipType::Invite:
                        if (e.repeatsState())
                            return tr("reinvited %1 to the room").arg(subjectName);
                        FALLTHROUGH;
                    case MembershipType::Join:
                    {
                        if (e.repeatsState())
                            return tr("joined the room (repeated)");
                        if (!e.prevContent() ||
                                e.membership() != e.prevContent()->membership)
                        {
                            return e.membership() == MembershipType::Invite
                                    ? tr("invited %1 to the room").arg(subjectName)
                                    : tr("joined the room");
                        }
                        QString text {};
                        if (e.displayName() != e.prevContent()->displayName)
                        {
                            if (e.displayName().isEmpty())
                                text = tr("cleared the display name");
                            else
                                text = tr("changed the display name to %1")
                                            .arg(e.displayName());
                        }
                        if (e.avatarUrl() != e.prevContent()->avatarUrl)
                        {
                            if (!text.isEmpty())
                                text += " and ";
                            if (e.avatarUrl().isEmpty())
                                text += tr("cleared the avatar");
                            else
                                text += tr("updated the avatar");
                        }
                        return text;
                    }
                    case MembershipType::Leave:
                        if (e.prevContent() &&
                                e.prevContent()->membership == MembershipType::Ban)
                        {
                            return (e.senderId() != e.userId())
                                    ? tr("unbanned %1").arg(subjectName)
                                    : tr("self-unbanned");
                        }
                        return (e.senderId() != e.userId())
                                ? tr("has put %1 out of the room").arg(subjectName)
                                : tr("left the room");
                    case MembershipType::Ban:
                        return (e.senderId() != e.userId())
                                ? tr("banned %1 from the room").arg(subjectName)
                                : tr("self-banned from the room");
                    case MembershipType::Knock:
                        return tr("knocked");
                    default:
                        ;
                }
                return tr("made something unknown");
            }
            , [] (const RoomAliasesEvent& e) {
                return tr("set aliases to: %1").arg(e.aliases().join(", "));
            }
            , [] (const RoomCanonicalAliasEvent& e) {
                return (e.alias().isEmpty())
                        ? tr("cleared the room main alias")
                        : tr("set the room main alias to: %1").arg(e.alias());
            }
            , [] (const RoomNameEvent& e) {
                return (e.name().isEmpty())
                        ? tr("cleared the room name")
                        : tr("set the room name to: %1").arg(e.name());
            }
            , [] (const RoomTopicEvent& e) {
                return (e.topic().isEmpty())
                        ? tr("cleared the topic")
                        : tr("set the topic to: %1").arg(e.topic());
            }
            , [] (const RoomAvatarEvent&) {
                return tr("changed the room avatar");
            }
            , [] (const EncryptionEvent&) {
                return tr("activated End-to-End Encryption");
            }
            , tr("Unknown Event")
        );
    }

    if( role == Qt::ToolTipRole )
    {
        return evt.originalJson();
    }

    if( role == EventTypeRole )
    {
        if (auto e = eventCast<const RoomMessageEvent>(&evt))
        {
            switch (e->msgtype())
            {
                case MessageEventType::Emote:
                    return "emote";
                case MessageEventType::Notice:
                    return "notice";
                case MessageEventType::Image:
                    return "image";
                case MessageEventType::File:
                case MessageEventType::Audio:
                case MessageEventType::Video:
                    return "file";
            default:
                return "message";
            }
        }
        if (is<RedactionEvent>(evt))
            return "redaction";
        if (evt.isStateEvent())
            return "state";

        return "other";
    }

    if (role == EventResolvedTypeRole)
        return EventTypeRegistry::getMatrixType(evt.type());

    if( role == AuthorRole )
    {
        // FIXME: It shouldn't be User, it should be its state "as of event"
        return QVariant::fromValue(row < timelineBaseIdx
                                   ? m_currentRoom->localUser()
                                   : m_currentRoom->user(evt.senderId()));
    }

    if (role == ContentTypeRole)
    {
        if (auto e = eventCast<const RoomMessageEvent>(&evt))
        {
            const auto& contentType = e->mimeType().name();
            return contentType == "text/plain"
                    ? QStringLiteral("text/html") : contentType;
        }
        return QStringLiteral("text/plain");
    }

    if (role == ContentRole)
    {
        if (evt.isRedacted())
        {
            auto reason = evt.redactedBecause()->reason();
            return (reason.isEmpty())
                    ? tr("Redacted")
                    : tr("Redacted: %1").arg(evt.redactedBecause()->reason());
        }

        if (auto e = eventCast<const RoomMessageEvent>(&evt))
        {
            // Cannot use e.contentJson() here because some
            // EventContent classes inject values into the copy of the
            // content JSON stored in EventContent::Base
            return e->hasFileContent()
                    ? QVariant::fromValue(e->content()->originalJson)
                    : QVariant();
        };
    }

    if( role == HighlightRole )
        return m_currentRoom->isEventHighlighted(&evt);

    if( role == ReadMarkerRole )
        return evt.id() == lastReadEventId;

    if( role == SpecialMarksRole )
    {
        if (row < timelineBaseIdx)
            return evt.id().isEmpty() ? "unsent" : "unsynced";

        if (evt.isStateEvent() &&
                static_cast<const StateEventBase&>(evt).repeatsState())
            return "noop";
        return evt.isRedacted() ? "redacted" : "";
    }

    if( role == EventIdRole )
        return evt.id();

    if( role == LongOperationRole )
    {
        if (auto e = eventCast<const RoomMessageEvent>(&evt))
            if (e->hasFileContent())
                return QVariant::fromValue(
                            m_currentRoom->fileTransferInfo(e->id()));
    }

    if (row >= timelineBaseIdx - 1) // The timeline and the topmost unsynced
    {
        if( role == TimeRole )
            return row < timelineBaseIdx ? QDateTime::currentDateTimeUtc()
                                         : makeMessageTimestamp(timelineIt);

        if( role == SectionRole )
            return row < timelineBaseIdx ? tr("Today")
                                         : makeDateString(timelineIt); // FIXME: move date rendering to QML

        // FIXME: shouldn't be here, because #312
        auto aboveEventIt = timelineIt + 1;
        if (aboveEventIt != m_currentRoom->timelineEdge())
        {
            if( role == AboveSectionRole )
                return makeDateString(aboveEventIt);

            if( role == AboveAuthorRole )
                return QVariant::fromValue(
                            m_currentRoom->user((*aboveEventIt)->senderId()));
        }
    }

    return QVariant();
}
