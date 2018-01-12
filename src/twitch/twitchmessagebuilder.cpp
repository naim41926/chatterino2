#include "twitchmessagebuilder.hpp"
#include "debug/log.hpp"
#include "singletons/emotemanager.hpp"
#include "singletons/ircmanager.hpp"
#include "singletons/resourcemanager.hpp"
#include "singletons/settingsmanager.hpp"
#include "singletons/thememanager.hpp"
#include "singletons/windowmanager.hpp"
#include "twitch/twitchchannel.hpp"

#include <QApplication>
#include <QDebug>
#include <QMediaPlayer>

using namespace chatterino::messages;

namespace chatterino {
namespace twitch {

TwitchMessageBuilder::TwitchMessageBuilder(Channel *_channel,
                                           const Communi::IrcPrivateMessage *_ircMessage,
                                           const messages::MessageParseArgs &_args)
    : channel(_channel)
    , twitchChannel(dynamic_cast<TwitchChannel *>(_channel))
    , ircMessage(_ircMessage)
    , args(_args)
    , tags(this->ircMessage->tags())
    , usernameColor(singletons::ThemeManager::getInstance().messages.textColors.system)
{
}

MessagePtr TwitchMessageBuilder::parse()
{
    singletons::SettingManager &settings = singletons::SettingManager::getInstance();
    singletons::EmoteManager &emoteManager = singletons::EmoteManager::getInstance();

    this->originalMessage = this->ircMessage->content();

    // PARSING
    this->parseUsername();

    //    this->message->setCollapsedDefault(true);
    //    this->appendWord(Word(Resources::getInstance().badgeCollapsed, Word::Collapsed, QString(),
    //    QString()));

    // The timestamp is always appended to the builder
    // Whether or not will be rendered is decided/checked later

    // Appends the correct timestamp if the message is a past message

    bool isPastMsg = this->tags.contains("historical");
    if (isPastMsg) {
        // This may be architecture dependent(datatype)
        qint64 ts = this->tags.value("tmi-sent-ts").toLongLong();
        QDateTime dateTime = QDateTime::fromMSecsSinceEpoch(ts);
        this->append<TimestampElement>(dateTime.time());
    } else {
        this->append<TimestampElement>();
    }

    this->parseMessageID();

    this->parseRoomID();

    // TIMESTAMP
    this->append<TwitchModerationElement>();

    this->parseTwitchBadges();

    this->addChatterinoBadges();

    if (this->args.includeChannelName) {
        this->parseChannelName();
    }

    this->appendUsername();

    // highlights
    if (settings.enableHighlights && !isPastMsg) {
        this->parseHighlights();
    }

    QString bits;
    auto iterator = this->tags.find("bits");
    if (iterator != this->tags.end()) {
        bits = iterator.value().toString();
    }

    // twitch emotes
    std::vector<std::pair<long, util::EmoteData>> twitchEmotes;

    iterator = this->tags.find("emotes");
    if (iterator != this->tags.end()) {
        QStringList emoteString = iterator.value().toString().split('/');

        for (QString emote : emoteString) {
            this->appendTwitchEmote(ircMessage, emote, twitchEmotes);
        }

        struct {
            bool operator()(const std::pair<long, util::EmoteData> &lhs,
                            const std::pair<long, util::EmoteData> &rhs)
            {
                return lhs.first < rhs.first;
            }
        } customLess;

        std::sort(twitchEmotes.begin(), twitchEmotes.end(), customLess);
    }

    auto currentTwitchEmote = twitchEmotes.begin();

    // words

    QStringList splits = this->originalMessage.split(' ');

    long int i = 0;

    for (QString split : splits) {
        MessageColor textColor = ircMessage->isAction() ? MessageColor(this->usernameColor)
                                                        : MessageColor(MessageColor::Text);

        // twitch emote
        if (currentTwitchEmote != twitchEmotes.end() && currentTwitchEmote->first == i) {
            auto emoteImage = currentTwitchEmote->second;
            this->append<EmoteElement>(emoteImage, MessageElement::TwitchEmote);

            i += split.length() + 1;
            currentTwitchEmote = std::next(currentTwitchEmote);

            continue;
        }

        // split words
        std::vector<std::tuple<util::EmoteData, QString>> parsed;

        // Parse emojis and take all non-emojis and put them in parsed as full text-words
        emoteManager.parseEmojis(parsed, split);

        for (const auto &tuple : parsed) {
            const util::EmoteData &emoteData = std::get<0>(tuple);

            if (!emoteData.isValid()) {  // is text
                QString string = std::get<1>(tuple);

                if (!bits.isEmpty() && this->tryParseCheermote(string)) {
                    // This string was parsed as a cheermote
                    continue;
                }

                // TODO: Implement ignored emotes
                // Format of ignored emotes:
                // Emote name: "forsenPuke" - if string in ignoredEmotes
                // Will match emote regardless of source (i.e. bttv, ffz)
                // Emote source + name: "bttv:nyanPls"
                if (this->tryAppendEmote(string)) {
                    // Successfully appended an emote
                    continue;
                }

                // Actually just text
                QString linkString = this->matchLink(string);

                Link link;

                if (linkString.isEmpty()) {
                    link = Link();
                } else {
                    link = Link(Link::Url, linkString);
                    textColor = MessageColor(MessageColor::Link);
                }

                this->append<TextElement>(string, EmoteElement::Text)  //
                    ->setLink(link);
            } else {  // is emoji
                this->append<EmoteElement>(emoteData, EmoteElement::EmojiAll);
            }
        }

        for (int j = 0; j < split.size(); j++) {
            i++;

            if (split.at(j).isHighSurrogate()) {
                j++;
            }
        }

        i++;
    }

    return this->getMessage();
}

void TwitchMessageBuilder::parseMessageID()
{
    auto iterator = this->tags.find("id");

    if (iterator != this->tags.end()) {
        this->messageID = iterator.value().toString();
    }
}

void TwitchMessageBuilder::parseRoomID()
{
    if (this->twitchChannel == nullptr) {
        return;
    }

    auto iterator = this->tags.find("room-id");

    if (iterator != std::end(this->tags)) {
        this->roomID = iterator.value().toString();

        if (this->twitchChannel->roomID.isEmpty()) {
            this->twitchChannel->roomID = this->roomID;
        }
    }
}

void TwitchMessageBuilder::parseChannelName()
{
    QString channelName("#" + this->channel->name);
    Link link(Link::Url, this->channel->name + "\n" + this->messageID);

    this->append<TextElement>(channelName, MessageElement::ChannelName, MessageColor::System)  //
        ->setLink(link);
}

void TwitchMessageBuilder::parseUsername()
{
    auto iterator = this->tags.find("color");
    if (iterator != this->tags.end()) {
        this->usernameColor = QColor(iterator.value().toString());
    }

    // username
    this->userName = ircMessage->nick();

    if (this->userName.isEmpty()) {
        this->userName = this->tags.value(QLatin1String("login")).toString();
    }

    this->message->loginName = this->userName;
}

void TwitchMessageBuilder::appendUsername()
{
    QString username = this->userName;
    QString localizedName;

    auto iterator = this->tags.find("display-name");
    if (iterator != this->tags.end()) {
        QString displayName = iterator.value().toString();

        if (QString::compare(displayName, this->userName, Qt::CaseInsensitive) == 0) {
            username = displayName;

            this->message->displayName = displayName;
        } else {
            localizedName = displayName;

            this->message->displayName = username;
            this->message->localizedName = displayName;
        }
    }

    bool hasLocalizedName = !localizedName.isEmpty();

    // The full string that will be rendered in the chat widget
    QString usernameText;

    pajlada::Settings::Setting<int> usernameDisplayMode(
        "/appearance/messages/usernameDisplayMode", UsernameDisplayMode::UsernameAndLocalizedName);

    switch (usernameDisplayMode.getValue()) {
        case UsernameDisplayMode::Username: {
            usernameText = username;
        } break;

        case UsernameDisplayMode::LocalizedName: {
            if (hasLocalizedName) {
                usernameText = localizedName;
            } else {
                usernameText = username;
            }
        } break;

        default:
        case UsernameDisplayMode::UsernameAndLocalizedName: {
            if (hasLocalizedName) {
                usernameText = username + "(" + localizedName + ")";
            } else {
                usernameText = username;
            }
        } break;
    }

    if (this->args.isSentWhisper) {
        // TODO(pajlada): Re-implement
        // userDisplayString += IrcManager::getInstance().getUser().getUserName();
    }

    if (this->args.isReceivedWhisper) {
        // TODO(pajlada): Re-implement
        // userDisplayString += " -> " + IrcManager::getInstance().getUser().getUserName();
    }

    if (!ircMessage->isAction()) {
        usernameText += ":";
    }

    this->append<TextElement>(usernameText, MessageElement::Text, this->usernameColor,
                              FontStyle::MediumBold)
        ->setLink({Link::UserInfo, this->userName});
}

void TwitchMessageBuilder::parseHighlights()
{
    static auto player = new QMediaPlayer;
    static QUrl currentPlayerUrl;
    singletons::SettingManager &settings = singletons::SettingManager::getInstance();
    static pajlada::Settings::Setting<std::string> currentUser("/accounts/current");

    QString currentUsername = QString::fromStdString(currentUser.getValue());

    if (this->ircMessage->nick() == currentUsername) {
        // Do nothing. Highlights cannot be triggered by yourself
        return;
    }

    // update the media player url if necessary
    QUrl highlightSoundUrl;
    if (settings.customHighlightSound) {
        highlightSoundUrl = QUrl(settings.pathHighlightSound.getValue());
    } else {
        highlightSoundUrl = QUrl("qrc:/sounds/ping2.wav");
    }

    if (currentPlayerUrl != highlightSoundUrl) {
        player->setMedia(highlightSoundUrl);

        currentPlayerUrl = highlightSoundUrl;
    }

    QStringList blackList =
        settings.highlightUserBlacklist.getValue().split("\n", QString::SkipEmptyParts);

    // TODO: This vector should only be rebuilt upon highlights being changed
    auto activeHighlights = settings.highlightProperties.getValue();

    if (settings.enableHighlightsSelf && currentUsername.size() > 0) {
        messages::HighlightPhrase selfHighlight;
        selfHighlight.key = currentUsername;
        selfHighlight.sound = settings.enableHighlightSound;
        selfHighlight.alert = settings.enableHighlightTaskbar;
        activeHighlights.emplace_back(std::move(selfHighlight));
    }

    bool doHighlight = false;
    bool playSound = false;
    bool doAlert = false;

    bool hasFocus = (QApplication::focusWidget() != nullptr);

    if (!blackList.contains(this->ircMessage->nick(), Qt::CaseInsensitive)) {
        for (const messages::HighlightPhrase &highlight : activeHighlights) {
            if (this->originalMessage.contains(highlight.key, Qt::CaseInsensitive)) {
                debug::Log("Highlight because {} contains {}", this->originalMessage,
                           highlight.key);
                doHighlight = true;

                if (highlight.sound) {
                    playSound = true;
                }

                if (highlight.alert) {
                    doAlert = true;
                }

                if (playSound && doAlert) {
                    // Break if no further action can be taken from other highlights
                    // This might change if highlights can have custom colors/sounds/actions
                    break;
                }
            }
        }

        this->setHighlight(doHighlight);

        if (playSound && (!hasFocus || settings.highlightAlwaysPlaySound)) {
            player->play();
        }

        if (doAlert) {
            QApplication::alert(singletons::WindowManager::getInstance().getMainWindow().window(),
                                2500);
        }

        if (doHighlight) {
            this->message->addFlags(Message::Highlighted);
        }
    }
}

void TwitchMessageBuilder::appendTwitchEmote(const Communi::IrcPrivateMessage *ircMessage,
                                             const QString &emote,
                                             std::vector<std::pair<long int, util::EmoteData>> &vec)
{
    singletons::EmoteManager &emoteManager = singletons::EmoteManager::getInstance();
    if (!emote.contains(':')) {
        return;
    }

    QStringList parameters = emote.split(':');

    if (parameters.length() < 2) {
        return;
    }

    long int id = std::stol(parameters.at(0).toStdString(), nullptr, 10);

    QStringList occurences = parameters.at(1).split(',');

    for (QString occurence : occurences) {
        QStringList coords = occurence.split('-');

        if (coords.length() < 2) {
            return;
        }

        long int start = std::stol(coords.at(0).toStdString(), nullptr, 10);
        long int end = std::stol(coords.at(1).toStdString(), nullptr, 10);

        if (start >= end || start < 0 || end > ircMessage->content().length()) {
            return;
        }

        QString name = ircMessage->content().mid(start, end - start + 1);

        vec.push_back(
            std::pair<long int, util::EmoteData>(start, emoteManager.getTwitchEmoteById(id, name)));
    }
}

bool TwitchMessageBuilder::tryAppendEmote(QString &emoteString)
{
    singletons::EmoteManager &emoteManager = singletons::EmoteManager::getInstance();
    util::EmoteData emoteData;

    if (emoteManager.bttvGlobalEmotes.tryGet(emoteString, emoteData)) {
        // BTTV Global Emote
        return this->appendEmote(emoteData);
    } else if (this->twitchChannel != nullptr &&
               this->twitchChannel->bttvChannelEmotes->tryGet(emoteString, emoteData)) {
        // BTTV Channel Emote
        return this->appendEmote(emoteData);
    } else if (emoteManager.ffzGlobalEmotes.tryGet(emoteString, emoteData)) {
        // FFZ Global Emote
        return this->appendEmote(emoteData);
    } else if (this->twitchChannel != nullptr &&
               this->twitchChannel->ffzChannelEmotes->tryGet(emoteString, emoteData)) {
        // FFZ Channel Emote
        return this->appendEmote(emoteData);
    } else if (emoteManager.getChatterinoEmotes().tryGet(emoteString, emoteData)) {
        // Chatterino Emote
        return this->appendEmote(emoteData);
    }

    return false;
}

bool TwitchMessageBuilder::appendEmote(const util::EmoteData &emoteData)
{
    this->append<EmoteElement>(emoteData, MessageElement::BttvEmote);

    // Perhaps check for ignored emotes here?
    return true;
}

// fourtf: this is ugly
//		   maybe put the individual badges into a map instead of this mess
void TwitchMessageBuilder::parseTwitchBadges()
{
    singletons::ResourceManager &resourceManager = singletons::ResourceManager::getInstance();
    const auto &channelResources = resourceManager.channels[this->roomID];

    auto iterator = this->tags.find("badges");

    if (iterator == this->tags.end()) {
        // No badges in this message
        return;
    }

    QStringList badges = iterator.value().toString().split(',');

    for (QString badge : badges) {
        if (badge.isEmpty()) {
            continue;
        }

        if (badge.startsWith("bits/")) {
            if (!singletons::ResourceManager::getInstance().dynamicBadgesLoaded) {
                // Do nothing
                continue;
            }

            QString cheerAmountQS = badge.mid(5);
            std::string versionKey = cheerAmountQS.toStdString();

            // Try to fetch channel-specific bit badge
            try {
                const auto &badge = channelResources.badgeSets.at("bits").versions.at(versionKey);
                this->append<ImageElement>(*(badge.badgeImage1x), MessageElement::BadgeVanity);
                continue;
            } catch (const std::out_of_range &) {
                // Channel does not contain a special bit badge for this version
            }

            // Use default bit badge
            try {
                const auto &badge = resourceManager.badgeSets.at("bits").versions.at(versionKey);
                this->append<ImageElement>(*(badge.badgeImage1x), MessageElement::BadgeVanity);
            } catch (const std::out_of_range &) {
                debug::Log("No default bit badge for version {} found", versionKey);
                continue;
            }
        } else if (badge == "staff/1") {
            this->append<ImageElement>(*resourceManager.badgeStaff,
                                       MessageElement::BadgeGlobalAuthority)
                ->setTooltip("Twitch Staff");
        } else if (badge == "admin/1") {
            this->append<ImageElement>(*resourceManager.badgeAdmin,
                                       MessageElement::BadgeGlobalAuthority)
                ->setTooltip("Twitch Admin");
        } else if (badge == "global_mod/1") {
            this->append<ImageElement>(*resourceManager.badgeGlobalModerator,
                                       MessageElement::BadgeGlobalAuthority)
                ->setTooltip("Twitch Global Moderator");
        } else if (badge == "moderator/1") {
            // TODO: Implement custom FFZ moderator badge
            this->append<ImageElement>(*resourceManager.badgeModerator,
                                       MessageElement::BadgeChannelAuthority)
                ->setTooltip("Twitch Channel Moderator");
        } else if (badge == "turbo/1") {
            this->append<ImageElement>(*resourceManager.badgeTurbo,
                                       MessageElement::BadgeGlobalAuthority)
                ->setTooltip("Twitch Turbo Subscriber");
        } else if (badge == "broadcaster/1") {
            this->append<ImageElement>(*resourceManager.badgeBroadcaster,
                                       MessageElement::BadgeChannelAuthority)
                ->setTooltip("Twitch Broadcaster");
        } else if (badge == "premium/1") {
            this->append<ImageElement>(*resourceManager.badgePremium, MessageElement::BadgeVanity)
                ->setTooltip("Twitch Prime Subscriber");
        } else if (badge.startsWith("partner/")) {
            int index = badge.midRef(8).toInt();
            switch (index) {
                case 1: {
                    this->append<ImageElement>(*resourceManager.badgeVerified,
                                               MessageElement::BadgeVanity)
                        ->setTooltip("Twitch Verified");
                } break;
                default: {
                    printf("[TwitchMessageBuilder] Unhandled partner badge index: %d\n", index);
                } break;
            }
        } else if (badge.startsWith("subscriber/")) {
            if (channelResources.loaded == false) {
                qDebug() << "Channel resources are not loaded, can't add the subscriber badge";
                continue;
            }

            auto badgeSetIt = channelResources.badgeSets.find("subscriber");
            if (badgeSetIt == channelResources.badgeSets.end()) {
                // Fall back to default badge
                this->append<ImageElement>(*resourceManager.badgeSubscriber,
                                           MessageElement::BadgeSubscription)
                    ->setTooltip("Twitch Subscriber");
                continue;
            }

            const auto &badgeSet = badgeSetIt->second;

            std::string versionKey = badge.mid(11).toStdString();

            auto badgeVersionIt = badgeSet.versions.find(versionKey);

            if (badgeVersionIt == badgeSet.versions.end()) {
                // Fall back to default badge
                this->append<ImageElement>(*resourceManager.badgeSubscriber,
                                           MessageElement::BadgeSubscription)
                    ->setTooltip("Twitch Subscriber");
                continue;
            }

            auto &badgeVersion = badgeVersionIt->second;

            this->append<ImageElement>(*badgeVersion.badgeImage1x,
                                       MessageElement::BadgeSubscription)
                ->setTooltip("Twitch " + QString::fromStdString(badgeVersion.title));
        } else {
            if (!resourceManager.dynamicBadgesLoaded) {
                // Do nothing
                continue;
            }

            QStringList parts = badge.split('/');

            if (parts.length() != 2) {
                qDebug() << "Bad number of parts: " << parts.length() << " in " << parts;
                continue;
            }

            MessageElement::Flags badgeType = MessageElement::Flags::BadgeVanity;

            std::string badgeSetKey = parts[0].toStdString();
            std::string versionKey = parts[1].toStdString();

            try {
                auto &badgeSet = resourceManager.badgeSets.at(badgeSetKey);

                try {
                    auto &badgeVersion = badgeSet.versions.at(versionKey);

                    this->append<ImageElement>(*badgeVersion.badgeImage1x, badgeType)
                        ->setTooltip("Twitch " + QString::fromStdString(badgeVersion.title));
                } catch (const std::exception &e) {
                    qDebug() << "Exception caught:" << e.what()
                             << "when trying to fetch badge version " << versionKey.c_str();
                }
            } catch (const std::exception &e) {
                qDebug() << "No badge set with key" << badgeSetKey.c_str()
                         << ". Exception: " << e.what();
            }
        }
    }
}

void TwitchMessageBuilder::addChatterinoBadges()
{
    auto &badges = singletons::ResourceManager::getInstance().chatterinoBadges;
    auto it = badges.find(this->userName.toStdString());

    if (it == badges.end()) {
        return;
    }

    const auto badge = it->second;

    this->append<ImageElement>(*badge->image, MessageElement::BadgeChatterino)
        ->setTooltip(QString::fromStdString(badge->tooltip));
}

bool TwitchMessageBuilder::tryParseCheermote(const QString &string)
{
    // Try to parse custom cheermotes
    const auto &channelResources =
        singletons::ResourceManager::getInstance().channels[this->roomID];
    if (channelResources.loaded) {
        for (const auto &cheermoteSet : channelResources.cheermoteSets) {
            auto match = cheermoteSet.regex.match(string);
            if (!match.hasMatch()) {
                continue;
            }
            QString amount = match.captured(1);
            bool ok = false;
            int numBits = amount.toInt(&ok);
            if (!ok) {
                debug::Log("Error parsing bit amount in tryParseCheermote");
                return false;
            }

            auto savedIt = cheermoteSet.cheermotes.end();

            // Fetch cheermote that matches our numBits
            for (auto it = cheermoteSet.cheermotes.begin(); it != cheermoteSet.cheermotes.end();
                 ++it) {
                if (numBits >= it->minBits) {
                    savedIt = it;
                } else {
                    break;
                }
            }

            if (savedIt == cheermoteSet.cheermotes.end()) {
                debug::Log("Error getting a cheermote from a cheermote set for the bit amount {}",
                           numBits);
                return false;
            }

            const auto &cheermote = *savedIt;

            this->append<EmoteElement>(cheermote.emoteDataAnimated, EmoteElement::BitsAnimated);
            this->append<TextElement>(amount, EmoteElement::Text, cheermote.color);

            return true;
        }
    }

    return false;
}

// bool
// sortTwitchEmotes(const std::pair<long int, Image *> &a,
//                 const std::pair<long int, Image *> &b)
//{
//    return a.first < b.first;
//}

}  // namespace twitch
}  // namespace chatterino
